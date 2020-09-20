/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 *
 *    Copyright 2018 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2018 (c) Thomas Stalder, Blue Time Concept SA
 *    Copyright 2019 (c) HMS Industrial Networks AB (Author: Jonas Green)
 */

#include "ua_session.h"
#include "ua_server_internal.h"
#ifdef UA_ENABLE_SUBSCRIPTIONS
#include "ua_subscription.h"
#endif

#define UA_SESSION_NONCELENTH 32

void UA_Session_init(UA_Session *session) {
    memset(session, 0, sizeof(UA_Session));
    session->availableContinuationPoints = UA_MAXCONTINUATIONPOINTS;
#ifdef UA_ENABLE_SUBSCRIPTIONS
    SIMPLEQ_INIT(&session->responseQueue);
#endif
}

void UA_Session_deleteMembersCleanup(UA_Session *session, UA_Server* server) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);
    UA_Session_detachFromSecureChannel(session);
    UA_ApplicationDescription_deleteMembers(&session->clientDescription);
    UA_NodeId_deleteMembers(&session->header.authenticationToken);
    UA_NodeId_deleteMembers(&session->sessionId);
    UA_String_deleteMembers(&session->sessionName);
    UA_ByteString_deleteMembers(&session->serverNonce);
    struct ContinuationPoint *cp, *next = session->continuationPoints;
    while((cp = next)) {
        next = ContinuationPoint_clear(cp);
        UA_free(cp);
    }
    session->continuationPoints = NULL;
    session->availableContinuationPoints = UA_MAXCONTINUATIONPOINTS;
}

void
UA_Session_attachToSecureChannel(UA_Session *session, UA_SecureChannel *channel) {
    UA_Session_detachFromSecureChannel(session);
    session->header.channel = channel;
    SLIST_INSERT_HEAD(&channel->sessions, &session->header, next);
}

void
UA_Session_detachFromSecureChannel(UA_Session *session) {
    UA_SecureChannel *channel = session->header.channel;
    if(!channel)
        return;
    session->header.channel = NULL;
    UA_SessionHeader *sh;
    SLIST_FOREACH(sh, &channel->sessions, next) {
        if((UA_Session*)sh != session)
            continue;
        SLIST_REMOVE(&channel->sessions, sh, UA_SessionHeader, next);
        break;
    }
}

UA_StatusCode
UA_Session_generateNonce(UA_Session *session) {
    UA_SecureChannel *channel = session->header.channel;
    if(!channel || !channel->securityPolicy)
        return UA_STATUSCODE_BADINTERNALERROR;

    /* Is the length of the previous nonce correct? */
    if(session->serverNonce.length != UA_SESSION_NONCELENTH) {
        UA_ByteString_deleteMembers(&session->serverNonce);
        UA_StatusCode retval =
            UA_ByteString_allocBuffer(&session->serverNonce, UA_SESSION_NONCELENTH);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
    }

    return channel->securityPolicy->symmetricModule.
        generateNonce(channel->securityPolicy, &session->serverNonce);
}

void UA_Session_updateLifetime(UA_Session *session) {
    session->validTill = UA_DateTime_nowMonotonic() +
        (UA_DateTime)(session->timeout * UA_DATETIME_MSEC);
}

#ifdef UA_ENABLE_SUBSCRIPTIONS

void
UA_Session_addSubscription(UA_Server *server, UA_Session *session,
                           UA_Subscription *newSubscription) {
    newSubscription->session = session;
    newSubscription->subscriptionId = ++server->lastSubscriptionId;
    LIST_INSERT_HEAD(&session->serverSubscriptions, newSubscription, listEntry);
    session->numSubscriptions++;
    server->numSubscriptions++;
}

UA_StatusCode
UA_Session_deleteSubscription(UA_Server *server, UA_Session *session,
                              UA_Subscription *sub) {
    UA_LOCK_ASSERT(server->serviceMutex, 1);

    UA_Subscription_clear(server, sub);

    /* Add a delayed callback to remove the subscription when the currently
     * scheduled jobs have completed. There is no actual delayed callback. Just
     * free the structure. */
    sub->delayedFreePointers.callback = NULL;
    UA_WorkQueue_enqueueDelayed(&server->workQueue, &sub->delayedFreePointers);

    /* Remove from the session */
    LIST_REMOVE(sub, listEntry);
    UA_assert(session->numSubscriptions > 0);
    UA_assert(server->numSubscriptions > 0);
    session->numSubscriptions--;
    server->numSubscriptions--;

    UA_LOG_INFO_SESSION(&server->config.logger, sub->session,
                        "Subscription %" PRIu32 " | Deleted the Subscription",
                        sub->subscriptionId);

    return UA_STATUSCODE_GOOD;
}

UA_Subscription *
UA_Session_getSubscriptionById(UA_Session *session, UA_UInt32 subscriptionId) {
    UA_Subscription *sub;
    LIST_FOREACH(sub, &session->serverSubscriptions, listEntry) {
        /* Prevent lookup of subscriptions that are to be deleted with a statuschange */
        if(sub->statusChange != UA_STATUSCODE_GOOD)
            continue;
        if(sub->subscriptionId == subscriptionId)
            break;
    }
    return sub;
}

UA_PublishResponseEntry*
UA_Session_dequeuePublishReq(UA_Session *session) {
    UA_PublishResponseEntry* entry = SIMPLEQ_FIRST(&session->responseQueue);
    if(entry) {
        SIMPLEQ_REMOVE_HEAD(&session->responseQueue, listEntry);
        session->numPublishReq--;
    }
    return entry;
}

void
UA_Session_queuePublishReq(UA_Session *session, UA_PublishResponseEntry* entry, UA_Boolean head) {
    if(!head)
        SIMPLEQ_INSERT_TAIL(&session->responseQueue, entry, listEntry);
    else
        SIMPLEQ_INSERT_HEAD(&session->responseQueue, entry, listEntry);
    session->numPublishReq++;
}

#endif
