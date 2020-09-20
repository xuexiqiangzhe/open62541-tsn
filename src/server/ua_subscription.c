/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2015-2018 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2015 (c) Chris Iatrou
 *    Copyright 2015-2016 (c) Sten Grüner
 *    Copyright 2017-2018 (c) Thomas Stalder, Blue Time Concept SA
 *    Copyright 2015 (c) Joakim L. Gilje
 *    Copyright 2016-2017 (c) Florian Palm
 *    Copyright 2015-2016 (c) Oleksiy Vasylyev
 *    Copyright 2017 (c) frax2222
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2017 (c) Ari Breitkreuz, fortiss GmbH
 *    Copyright 2017 (c) Mattias Bornhager
 *    Copyright 2018 (c) Hilscher Gesellschaft für Systemautomation mbH (Author: Martin Lang)
 *    Copyright 2019 (c) HMS Industrial Networks AB (Author: Jonas Green)
 */

#include "ua_server_internal.h"
#include "ua_subscription.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS /* conditional compilation */

UA_Subscription *
UA_Subscription_new() {
    /* Allocate the memory */
    UA_Subscription *newSub = (UA_Subscription*)UA_calloc(1, sizeof(UA_Subscription));
    if(!newSub)
        return NULL;

    /* The first publish response is sent immediately */
    newSub->state = UA_SUBSCRIPTIONSTATE_NORMAL;

    /* Even if the first publish response is a keepalive the sequence number is 1.
     * This can happen by a subscription without a monitored item (see CTT test scripts). */
    newSub->nextSequenceNumber = 1;

    TAILQ_INIT(&newSub->retransmissionQueue);
    TAILQ_INIT(&newSub->notificationQueue);
    return newSub;
}

void
UA_Subscription_clear(UA_Server *server, UA_Subscription *sub) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    Subscription_unregisterPublishCallback(server, sub);

    /* Delete monitored Items */
    UA_MonitoredItem *mon, *tmp_mon;
    LIST_FOREACH_SAFE(mon, &sub->monitoredItems, listEntry, tmp_mon) {
        LIST_REMOVE(mon, listEntry);
        UA_LOG_INFO_SESSION(&server->config.logger, sub->session,
                            "Subscription %" PRIu32 " | MonitoredItem %" PRIi32 " | "
                            "Deleted the MonitoredItem", sub->subscriptionId,
                            mon->monitoredItemId);
        UA_MonitoredItem_delete(server, mon);
    }
    UA_assert(server->numMonitoredItems >= sub->monitoredItemsSize);
    server->numMonitoredItems -= sub->monitoredItemsSize;
    sub->monitoredItemsSize = 0;

    /* Delete Retransmission Queue */
    UA_NotificationMessageEntry *nme, *nme_tmp;
    TAILQ_FOREACH_SAFE(nme, &sub->retransmissionQueue, listEntry, nme_tmp) {
        TAILQ_REMOVE(&sub->retransmissionQueue, nme, listEntry);
        UA_NotificationMessage_clear(&nme->message);
        UA_free(nme);
        --sub->session->totalRetransmissionQueueSize;
        --sub->retransmissionQueueSize;
    }
    UA_assert(sub->retransmissionQueueSize == 0);
}

UA_MonitoredItem *
UA_Subscription_getMonitoredItem(UA_Subscription *sub, UA_UInt32 monitoredItemId) {
    UA_MonitoredItem *mon;
    LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
        if(mon->monitoredItemId == monitoredItemId)
            break;
    }
    return mon;
}

UA_StatusCode
UA_Subscription_deleteMonitoredItem(UA_Server *server, UA_Subscription *sub,
                                    UA_UInt32 monitoredItemId) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    /* Find the MonitoredItem */
    UA_MonitoredItem *mon;
    LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
        if(mon->monitoredItemId == monitoredItemId)
            break;
    }
    if(!mon)
        return UA_STATUSCODE_BADMONITOREDITEMIDINVALID;

    UA_LOG_INFO_SESSION(&server->config.logger, sub->session,
                        "Subscription %" PRIu32 " | MonitoredItem %" PRIi32 " | "
                        "Delete the MonitoredItem", sub->subscriptionId,
                        mon->monitoredItemId);

    /* Remove the MonitoredItem */
    LIST_REMOVE(mon, listEntry);
    UA_assert(sub->monitoredItemsSize > 0);
    UA_assert(server->numMonitoredItems > 0);
    sub->monitoredItemsSize--;
    server->numMonitoredItems--;

    /* Remove content and delayed free */
    UA_MonitoredItem_delete(server, mon);

    return UA_STATUSCODE_GOOD;
}

void
UA_Subscription_addMonitoredItem(UA_Server *server, UA_Subscription *sub, UA_MonitoredItem *newMon) {
    sub->monitoredItemsSize++;
    server->numMonitoredItems++;
    LIST_INSERT_HEAD(&sub->monitoredItems, newMon, listEntry);
}

static void
removeOldestRetransmissionMessage(UA_Session *session) {
    UA_NotificationMessageEntry *oldestEntry = NULL;
    UA_Subscription *oldestSub = NULL;

    UA_Subscription *sub;
    LIST_FOREACH(sub, &session->serverSubscriptions, listEntry) {
        UA_NotificationMessageEntry *first =
            TAILQ_LAST(&sub->retransmissionQueue, ListOfNotificationMessages);
        if(!first)
            continue;
        if(!oldestEntry || oldestEntry->message.publishTime > first->message.publishTime) {
            oldestEntry = first;
            oldestSub = sub;
        }
    }
    UA_assert(oldestEntry);
    UA_assert(oldestSub);

    TAILQ_REMOVE(&oldestSub->retransmissionQueue, oldestEntry, listEntry);
    UA_NotificationMessage_clear(&oldestEntry->message);
    UA_free(oldestEntry);
    --session->totalRetransmissionQueueSize;
    --oldestSub->retransmissionQueueSize;
}

static void
UA_Subscription_addRetransmissionMessage(UA_Server *server, UA_Subscription *sub,
                                         UA_NotificationMessageEntry *entry) {
    /* Release the oldest entry if there is not enough space */
    if(server->config.maxRetransmissionQueueSize > 0 &&
       sub->session->totalRetransmissionQueueSize >= server->config.maxRetransmissionQueueSize) {
        UA_LOG_WARNING_SESSION(&server->config.logger, sub->session, "Subscription %" PRIu32 " | "
                               "Retransmission queue overflow", sub->subscriptionId);
        removeOldestRetransmissionMessage(sub->session);
    }

    /* Add entry */
    TAILQ_INSERT_TAIL(&sub->retransmissionQueue, entry, listEntry);
    ++sub->session->totalRetransmissionQueueSize;
    ++sub->retransmissionQueueSize;
}

UA_StatusCode
UA_Subscription_removeRetransmissionMessage(UA_Subscription *sub, UA_UInt32 sequenceNumber) {
    /* Find the retransmission message */
    UA_NotificationMessageEntry *entry;
    TAILQ_FOREACH(entry, &sub->retransmissionQueue, listEntry) {
        if(entry->message.sequenceNumber == sequenceNumber)
            break;
    }
    if(!entry)
        return UA_STATUSCODE_BADSEQUENCENUMBERUNKNOWN;

    /* Remove the retransmission message */
    TAILQ_REMOVE(&sub->retransmissionQueue, entry, listEntry);
    --sub->session->totalRetransmissionQueueSize;
    --sub->retransmissionQueueSize;
    UA_NotificationMessage_clear(&entry->message);
    UA_free(entry);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
prepareNotificationMessage(UA_Server *server, UA_Subscription *sub,
                           UA_NotificationMessage *message, size_t notifications) {
    UA_assert(notifications > 0);

    /* Allocate an ExtensionObject for Event- and DataChange-Notifications. Also
     * there can be StatusChange-Notifications. The standard says in Part 4,
     * 7.2.1:
     *
     * If a Subscription contains MonitoredItems for events and data, this array
     * should have not more than 2 elements. */
    message->notificationData = (UA_ExtensionObject*)
        UA_Array_new(2, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
    if(!message->notificationData)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    message->notificationDataSize = 2;

    /* Pre-allocate DataChangeNotifications */
    size_t notificationDataIdx = 0;
    UA_DataChangeNotification *dcn = NULL;
    size_t dcnPos = 0; /* How many DataChangeNotifications were moved into the list? */
    if(sub->dataChangeNotifications > 0) {
        dcn = UA_DataChangeNotification_new();
        if(!dcn) {
            UA_NotificationMessage_clear(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        message->notificationData->encoding = UA_EXTENSIONOBJECT_DECODED;
        message->notificationData->content.decoded.data = dcn;
        message->notificationData->content.decoded.type = &UA_TYPES[UA_TYPES_DATACHANGENOTIFICATION];

        size_t dcnSize = sub->dataChangeNotifications;
        if(dcnSize > notifications)
            dcnSize = notifications;
        dcn->monitoredItems = (UA_MonitoredItemNotification*)
            UA_Array_new(dcnSize, &UA_TYPES[UA_TYPES_MONITOREDITEMNOTIFICATION]);
        if(!dcn->monitoredItems) {
            UA_NotificationMessage_clear(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        dcn->monitoredItemsSize = dcnSize;
        notificationDataIdx++;
    }

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    UA_EventNotificationList *enl = NULL;
    size_t enlPos = 0; /* How many EventNotifications were moved into the list? */
    if(sub->eventNotifications > 0) {
        enl = UA_EventNotificationList_new();
        if(!enl) {
            UA_NotificationMessage_clear(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        message->notificationData[notificationDataIdx].encoding = UA_EXTENSIONOBJECT_DECODED;
        message->notificationData[notificationDataIdx].content.decoded.data = enl;
        message->notificationData[notificationDataIdx].content.decoded.type =
            &UA_TYPES[UA_TYPES_EVENTNOTIFICATIONLIST];

        size_t enlSize = sub->eventNotifications;
        if(enlSize > notifications)
            enlSize = notifications;
        enl->events = (UA_EventFieldList*) UA_Array_new(enlSize, &UA_TYPES[UA_TYPES_EVENTFIELDLIST]);
        if(!enl->events) {
            UA_NotificationMessage_clear(message);
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
        enl->eventsSize = enlSize;
        notificationDataIdx++;
    }
#endif

    UA_assert(notificationDataIdx > 0);
    message->notificationDataSize = notificationDataIdx;

    /* <-- The point of no return --> */

    size_t totalNotifications = 0; /* How many notifications were moved to the response overall? */
    UA_Notification *notification, *notification_tmp;
    TAILQ_FOREACH_SAFE(notification, &sub->notificationQueue, globalEntry, notification_tmp) {
        if(totalNotifications >= notifications)
            break;

        /* Remove from the queues and decrease the counters */
        UA_Notification_dequeue(server, notification);

        /* Move the content to the response */
        switch(notification->mon->attributeId) {
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
        case UA_ATTRIBUTEID_EVENTNOTIFIER:
            UA_assert(enl != NULL); /* Have at least one event notification */
            enl->events[enlPos] = notification->data.event;
            UA_EventFieldList_init(&notification->data.event);
            enlPos++;
            break;
#endif
        default:
            UA_assert(dcn != NULL); /* Have at least one change notification */
            dcn->monitoredItems[dcnPos] = notification->data.dataChange;
            UA_DataValue_init(&notification->data.dataChange.value);
            dcnPos++;
            break;
        }

        UA_Notification_delete(notification);
        totalNotifications++;
    }

    /* Set sizes */
    if(dcn) {
        dcn->monitoredItemsSize = dcnPos;
        if(dcnPos == 0) {
            UA_free(dcn->monitoredItems);
            dcn->monitoredItems = NULL;
        }
    }

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    if(enl) {
        enl->eventsSize = enlPos;
        if(enlPos == 0) {
            UA_free(enl->events);
            enl->events = NULL;
        }
    }
#endif

    return UA_STATUSCODE_GOOD;
}

/* According to OPC Unified Architecture, Part 4 5.13.1.1 i) The value 0 is
 * never used for the sequence number */
static UA_UInt32
UA_Subscription_nextSequenceNumber(UA_UInt32 sequenceNumber) {
    UA_UInt32 nextSequenceNumber = sequenceNumber + 1;
    if(nextSequenceNumber == 0)
        nextSequenceNumber = 1;
    return nextSequenceNumber;
}

static void
publishCallback(UA_Server *server, UA_Subscription *sub) {
    sub->readyNotifications = sub->notificationQueueSize;
    UA_LOCK(server->serviceMutex);
    UA_Subscription_publish(server, sub);
    UA_UNLOCK(server->serviceMutex);
}

void
UA_Subscription_publish(UA_Server *server, UA_Subscription *sub) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session, "Subscription %" PRIu32 " | "
                         "Publish Callback", sub->subscriptionId);
    /* Dequeue a response */
    UA_PublishResponseEntry *pre = UA_Session_dequeuePublishReq(sub->session);
    if(pre) {
        sub->currentLifetimeCount = 0; /* Reset the LifetimeCounter */
    } else {
        UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                             "Subscription %" PRIu32 " | The publish queue is empty",
                             sub->subscriptionId);
        ++sub->currentLifetimeCount;

        if(sub->currentLifetimeCount > sub->lifeTimeCount) {
            UA_LOG_WARNING_SESSION(&server->config.logger, sub->session,
                                   "Subscription %" PRIu32 " | End of lifetime "
                                   "for subscription", sub->subscriptionId);
            /* Set the StatusChange to delete the subscription. */
            sub->statusChange = UA_STATUSCODE_BADTIMEOUT;
        }
    }

    UA_PublishResponse *response = &pre->response;
    UA_NotificationMessage *message = &response->notificationMessage;

    /* Send a StatusChange Notification and delete the Subscription. */
    if(sub->statusChange != UA_STATUSCODE_GOOD) {
        /* Cannot send out the StatusChange. Keep the "shell" of the
         * subscription to answer with a StatusChangeNotification when a Publish
         * Request is available. */
        if(!pre) {
            /* Remove the cyclic publich callback, the MonitoredItems and queued
             * Notifications */
            UA_Subscription_clear(server, sub);
            sub->state = UA_SUBSCRIPTIONSTATE_LATE;
            return;
        }

        UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                             "Subscription %" PRIu32 " | Sending out a StatusChange "
                             "notification and removing the subscription", sub->subscriptionId);

        /* Populate the response */
        UA_StatusChangeNotification scn;
        UA_StatusChangeNotification_init(&scn);
        scn.status = sub->statusChange;

        UA_ExtensionObject notificationData;
        UA_ExtensionObject_init(&notificationData);
        notificationData.encoding = UA_EXTENSIONOBJECT_DECODED;
        notificationData.content.decoded.type = &UA_TYPES[UA_TYPES_STATUSCHANGENOTIFICATION];
        notificationData.content.decoded.data = &scn;

        response->responseHeader.timestamp = UA_DateTime_now();
        response->notificationMessage.notificationData = &notificationData;
        response->notificationMessage.notificationDataSize = 1;
        response->subscriptionId = sub->subscriptionId;
        response->notificationMessage.publishTime = response->responseHeader.timestamp;
        response->notificationMessage.sequenceNumber = sub->nextSequenceNumber;

        /* Send the response */
        UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                             "Subscription %" PRIu32 " | Sending out a publish response.",
                                sub->subscriptionId);
        sendResponse(server, sub->session, sub->session->header.channel, pre->requestId,
                     (UA_Response*)response, &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);

        /* Clean up */
        response->notificationMessage.notificationData = NULL;
        response->notificationMessage.notificationDataSize = 0;
        UA_PublishResponse_clear(&pre->response);
        UA_free(pre);

        /* Delete the subscription */
        UA_Session_deleteSubscription(server, sub->session, sub);
        return;
    }

    /* If there are several late publish responses... */
    if(sub->readyNotifications > sub->notificationQueueSize)
        sub->readyNotifications = sub->notificationQueueSize;

    /* Count the available notifications */
    UA_UInt32 notifications = sub->readyNotifications;
    if(!sub->publishingEnabled)
        notifications = 0;

    UA_Boolean moreNotifications = false;
    if(notifications > sub->notificationsPerPublish) {
        notifications = sub->notificationsPerPublish;
        moreNotifications = true;
    }

    /* Return if no notifications and no keepalive */
    if(notifications == 0) {
        ++sub->currentKeepAliveCount;
        if(sub->currentKeepAliveCount < sub->maxKeepAliveCount) {
            if(pre)
                UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
            return;
        }
        UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                             "Subscription %" PRIu32 " | Sending a KeepAlive",
                             sub->subscriptionId);
    }

    /* We want to send a response. Is the channel open? */
    UA_SecureChannel *channel = sub->session->header.channel;
    if(!channel || !pre) {
        UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                             "Subscription %" PRIu32 " | "
                             "Want to send a publish response but can't. "
                             "The subscription is late.", sub->subscriptionId);
        sub->state = UA_SUBSCRIPTIONSTATE_LATE;
        if(pre)
            UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
        return;
    }

    /* Prepare the response */
    UA_NotificationMessageEntry *retransmission = NULL;
    if(notifications > 0) {
        if(server->config.enableRetransmissionQueue) {
            /* Allocate the retransmission entry */
            retransmission = (UA_NotificationMessageEntry*)
                UA_malloc(sizeof(UA_NotificationMessageEntry));
            if(!retransmission) {
                UA_LOG_WARNING_SESSION(&server->config.logger, sub->session,
                                       "Subscription %" PRIu32 " | "
                                       "Could not allocate memory for retransmission. "
                                       "The subscription is late.", sub->subscriptionId);
                sub->state = UA_SUBSCRIPTIONSTATE_LATE;
                UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
                return;
            }
        }

        /* Prepare the response */
        UA_StatusCode retval = prepareNotificationMessage(server, sub, message, notifications);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING_SESSION(&server->config.logger, sub->session,
                                   "Subscription %" PRIu32 " | "
                                   "Could not prepare the notification message. "
                                   "The subscription is late.", sub->subscriptionId);
            /* If the retransmission queue is enabled a retransmission message is allocated */
            if(retransmission)
                UA_free(retransmission);
            sub->state = UA_SUBSCRIPTIONSTATE_LATE;
            UA_Session_queuePublishReq(sub->session, pre, true); /* Re-enqueue */
            return;
        }
    }

    /* <-- The point of no return --> */

    /* Adjust the number of ready notifications */
    UA_assert(sub->readyNotifications >= notifications);
    sub->readyNotifications -= notifications;

    /* Set up the response */
    response->responseHeader.timestamp = UA_DateTime_now();
    response->subscriptionId = sub->subscriptionId;
    response->moreNotifications = moreNotifications;
    message->publishTime = response->responseHeader.timestamp;

    /* Set sequence number to message. Started at 1 which is given during
     * creating a new subscription. The 1 is required for initial publish
     * response with or without an monitored item. */
    message->sequenceNumber = sub->nextSequenceNumber;

    if(notifications > 0) {
        /* If the retransmission queue is enabled a retransmission message is
         * allocated */
        if(retransmission) {
            /* Put the notification message into the retransmission queue. This
             * needs to be done here, so that the message itself is included in
             * the available sequence numbers for acknowledgement. */
            retransmission->message = response->notificationMessage;
            UA_Subscription_addRetransmissionMessage(server, sub, retransmission);
        }
        /* Only if a notification was created, the sequence number must be
         * increased. For a keepalive the sequence number can be reused. */
        sub->nextSequenceNumber = UA_Subscription_nextSequenceNumber(sub->nextSequenceNumber);
    }

    /* Get the available sequence numbers from the retransmission queue */
    size_t available = sub->retransmissionQueueSize;
    UA_STACKARRAY(UA_UInt32, seqNumbers, available);
    if(available > 0) {
        response->availableSequenceNumbers = seqNumbers;
        response->availableSequenceNumbersSize = available;
        size_t i = 0;
        UA_NotificationMessageEntry *nme;
        TAILQ_FOREACH(nme, &sub->retransmissionQueue, listEntry) {
            response->availableSequenceNumbers[i] = nme->message.sequenceNumber;
            ++i;
        }
    }

    /* Send the response */
    UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                         "Subscription %" PRIu32 " | Sending out a publish response "
                         "with %" PRIu32 " notifications", sub->subscriptionId,
                         notifications);
    sendResponse(server, sub->session, sub->session->header.channel, pre->requestId,
                 (UA_Response*)response, &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);

    /* Reset subscription state to normal */
    sub->state = UA_SUBSCRIPTIONSTATE_NORMAL;
    sub->currentKeepAliveCount = 0;

    /* Free the response */
    if(retransmission)
        /* NotificationMessage was moved into retransmission queue */
        UA_NotificationMessage_init(&response->notificationMessage);
    response->availableSequenceNumbers = NULL;
    response->availableSequenceNumbersSize = 0;
    UA_PublishResponse_clear(&pre->response);
    UA_free(pre);

    /* Repeat sending responses if there are more notifications to send */
    if(moreNotifications)
        UA_Subscription_publish(server, sub);
}

UA_Boolean
UA_Subscription_reachedPublishReqLimit(UA_Server *server,  UA_Session *session) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                         "Reached number of publish request limit");

    /* Dequeue a response */
    UA_PublishResponseEntry *pre = UA_Session_dequeuePublishReq(session);

    /* Cannot publish without a response */
    if(!pre) {
        UA_LOG_FATAL_SESSION(&server->config.logger, session, "No publish requests available");
        return false;
    }

    /* <-- The point of no return --> */

    UA_PublishResponse *response = &pre->response;
    UA_NotificationMessage *message = &response->notificationMessage;

    /* Set up the response. Note that this response has no related subscription id */
    response->responseHeader.timestamp = UA_DateTime_now();
    response->responseHeader.serviceResult = UA_STATUSCODE_BADTOOMANYPUBLISHREQUESTS;
    response->subscriptionId = 0;
    response->moreNotifications = false;
    message->publishTime = response->responseHeader.timestamp;
    message->sequenceNumber = 0;
    response->availableSequenceNumbersSize = 0;

    /* Send the response */
    UA_LOG_DEBUG_SESSION(&server->config.logger, session,
                         "Sending out a publish response triggered by too many publish requests");
    sendResponse(server, session, session->header.channel, pre->requestId,
                 (UA_Response*)response, &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);

    /* Free the response */
    UA_Array_delete(response->results, response->resultsSize, &UA_TYPES[UA_TYPES_UINT32]);
    UA_free(pre); /* no need for UA_PublishResponse_clear */

    return true;
}

UA_StatusCode
Subscription_registerPublishCallback(UA_Server *server, UA_Subscription *sub) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session,
                         "Subscription %" PRIu32 " | Register subscription "
                         "publishing callback", sub->subscriptionId);
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    if(sub->publishCallbackIsRegistered)
        return UA_STATUSCODE_GOOD;

    UA_StatusCode retval =
        addRepeatedCallback(server, (UA_ServerCallback)publishCallback,
                            sub, sub->publishingInterval, &sub->publishCallbackId);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    sub->publishCallbackIsRegistered = true;
    return UA_STATUSCODE_GOOD;
}

void
Subscription_unregisterPublishCallback(UA_Server *server, UA_Subscription *sub) {
    UA_LOG_DEBUG_SESSION(&server->config.logger, sub->session, "Subscription %" PRIu32 " | "
                         "Unregister subscription publishing callback", sub->subscriptionId);

    if(!sub->publishCallbackIsRegistered)
        return;

    removeCallback(server, sub->publishCallbackId);
    sub->publishCallbackIsRegistered = false;
}

/* When the session has publish requests stored but the last subscription is
 * deleted... Send out empty responses */
void
UA_Subscription_answerPublishRequestsNoSubscription(UA_Server *server, UA_Session *session) {
    /* No session or there are remaining subscriptions */
    if(!session || LIST_FIRST(&session->serverSubscriptions))
        return;

    /* Send a response for every queued request */
    UA_PublishResponseEntry *pre;
    while((pre = UA_Session_dequeuePublishReq(session))) {
        UA_PublishResponse *response = &pre->response;
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOSUBSCRIPTION;
        response->responseHeader.timestamp = UA_DateTime_now();
        sendResponse(server, session, session->header.channel, pre->requestId,
                     (UA_Response*)response, &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);
        UA_PublishResponse_clear(response);
        UA_free(pre);
    }
}

#endif /* UA_ENABLE_SUBSCRIPTIONS */
