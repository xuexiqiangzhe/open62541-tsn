/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

/**
 * **Trace point setup**
 *
 *            +--------------+                        +----------------+
 *         T1 | OPCUA PubSub |  T8                 T5 | OPCUA loopback |  T4
 *         |  |  Application |  ^                  |  |  Application   |  ^
 *         |  +--------------+  |                  |  +----------------+  |
 *  User   |  |              |  |                  |  |                |  |
 *  Space  |  |              |  |                  |  |                |  |
 *         |  |              |  |                  |  |                |  |
 *------------|--------------|------------------------|----------------|--------
 *         |  |    Node 1    |  |                  |  |     Node 2     |  |
 *  Kernel |  |              |  |                  |  |                |  |
 *  Space  |  |              |  |                  |  |                |  |
 *         |  |              |  |                  |  |                |  |
 *         v  +--------------+  |                  v  +----------------+  |
 *         T2 |  TX tcpdump  |  T7<----------------T6 |   RX tcpdump   |  T3
 *         |  +--------------+                        +----------------+  ^
 *         |                                                              |
 *         ----------------------------------------------------------------
 */
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>
#include <sys/io.h>

/* For thread operations */
#include <pthread.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <ua_server_internal.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/plugin/log.h>
#include <open62541/types_generated.h>
#include <open62541/plugin/pubsub_ethernet.h>
#include <open62541/plugin/pubsub_ethernet_etf.h>

#include "ua_pubsub.h"

#ifdef UA_ENABLE_PUBSUB_ETH_UADP_XDP
#include <open62541/plugin/pubsub_ethernet_xdp.h>
#include <linux/if_link.h>
#endif

UA_NodeId readerGroupIdentifier;
UA_NodeId readerIdentifier;

UA_DataSetReaderConfig readerConfig;

/* to find load of each thread
 * ps -L -o pid,pri,%cpu -C pubsub_TSN_publisher */

/* Configurable Parameters */
/* These defines enables the publisher and subscriber of the OPCUA stack */
/* To run only publisher, enable PUBLISHER define alone (comment SUBSCRIBER) */
#define             PUBLISHER
/* To run only subscriber, enable SUBSCRIBER define alone (comment PUBLISHER) */
#define             SUBSCRIBER
#define             UPDATE_MEASUREMENTS
/* Cycle time in milliseconds */
#define             CYCLE_TIME                            0.25
/* Qbv offset */
#define             QBV_OFFSET                            25 * 1000
#define             SOCKET_PRIORITY                       3
#if defined(PUBLISHER)
#define             PUBLISHER_ID                          2234
#define             WRITER_GROUP_ID                       101
#define             DATA_SET_WRITER_ID                    62541
#define             PUBLISHING_MAC_ADDRESS                "opc.eth://01-00-5E-7F-00-01"
#endif
#if defined(SUBSCRIBER)
#define             PUBLISHER_ID_SUB                      2235
#define             WRITER_GROUP_ID_SUB                   100
#define             DATA_SET_WRITER_ID_SUB                62541
#define             SUBSCRIBING_MAC_ADDRESS               "opc.eth://01-00-5E-00-00-01"
#endif
#define             REPEATED_NODECOUNTS                   0
#define             PORT_NUMBER                           62541
#define             RECEIVE_QUEUE                         2
#define             XDP_FLAG                              XDP_FLAGS_SKB_MODE
#define             PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS

/* Non-Configurable Parameters */
/* Milli sec and sec conversion to nano sec */
#define             MILLI_SECONDS                         1000 * 1000
#define             SECONDS                               1000 * 1000 * 1000
#define             SECONDS_SLEEP                         5
/* Publisher will sleep for 60% of cycle time and then prepares the */
/* transmission packet within 40% */
#define             NANO_SECONDS_SLEEP_PUB                CYCLE_TIME * MILLI_SECONDS * 0.6
/* Subscriber will wakeup only during start of cycle and check whether */
/* the packets are received */
#define             NANO_SECONDS_SLEEP_SUB                0
/* User application Pub/Sub will wakeup at the 30% of cycle time and handles the */
/* user data such as read and write in Information model */
#define             NANO_SECONDS_SLEEP_USER_APPLICATION   CYCLE_TIME * MILLI_SECONDS * 0.3
/* Priority of Publisher, Subscriber, User application and server are kept */
/* after some prototyping and analyzing it */
#define             PUB_SCHED_PRIORITY                    78
#define             SUB_SCHED_PRIORITY                    81
#define             USERAPPLICATION_SCHED_PRIORITY        75
#define             SERVER_SCHED_PRIORITY                 1
#define             MAX_MEASUREMENTS                      30000000
#define             CORE_TWO                              2
#define             CORE_THREE                            3
#define             SECONDS_INCREMENT                     1
#define             CLOCKID                               CLOCK_TAI
#define             ETH_TRANSPORT_PROFILE                 "http://opcfoundation.org/UA-Profile/Transport/pubsub-eth-uadp"

/* If the Hardcoded publisher/subscriber MAC addresses need to be changed,
 * change PUBLISHING_MAC_ADDRESS and SUBSCRIBING_MAC_ADDRESS
 */

/* Set server running as true */
UA_Boolean          running                = UA_TRUE;
/* Variables corresponding to PubSub connection creation,
 * published data set and writer group */
UA_NodeId           connectionIdent;
UA_NodeId           publishedDataSetIdent;
UA_NodeId           writerGroupIdent;
UA_NodeId           pubNodeID;
UA_NodeId           subNodeID;
UA_NodeId           pubRepeatedCountNodeID;
UA_NodeId           subRepeatedCountNodeID;
/* Variables for counter data handling in address space */
UA_UInt64           *pubCounterData;
UA_UInt64           *repeatedCounterData[REPEATED_NODECOUNTS];
UA_DataValue        *staticValueSource;

UA_UInt64           subCounterData = 0;

#if defined(PUBLISHER)
#if defined(UPDATE_MEASUREMENTS)
/* File to store the data and timestamps for different traffic */
FILE               *fpPublisher;
char               *filePublishedData      = "publisher_T1.csv";
/* Array to store published counter data */
UA_UInt64           publishCounterValue[MAX_MEASUREMENTS];
size_t              measurementsPublisher  = 0;
/* Array to store timestamp */
struct timespec     publishTimestamp[MAX_MEASUREMENTS];
#endif
/* Thread for publisher */
pthread_t           pubthreadID;
struct timespec     dataModificationTime;
#endif

#if defined(SUBSCRIBER)
#if defined(UPDATE_MEASUREMENTS)
/* File to store the data and timestamps for different traffic */
FILE               *fpSubscriber;
char               *fileSubscribedData     = "subscriber_T8.csv";
/* Array to store subscribed counter data */
UA_UInt64           subscribeCounterValue[MAX_MEASUREMENTS];
size_t              measurementsSubscriber = 0;
/* Array to store timestamp */
struct timespec     subscribeTimestamp[MAX_MEASUREMENTS];
#endif
/* Thread for subscriber */
pthread_t           subthreadID;
/* Variable for PubSub connection creation */
UA_NodeId           connectionIdentSubscriber;
struct timespec     dataReceiveTime;
#endif

/* Thread for user application*/
pthread_t           userApplicationThreadID;

typedef struct {
UA_Server*                   ServerRun;
} serverConfigStruct;

/* Structure to define thread parameters */
typedef struct {
UA_Server*                   server;
void*                        data;
UA_ServerCallback            callback;
UA_Duration                  interval_ms;
UA_UInt64*                   callbackId;
} threadArg;

enum txtime_flags {
    SOF_TXTIME_DEADLINE_MODE = (1 << 0),
    SOF_TXTIME_REPORT_ERRORS = (1 << 1),

    SOF_TXTIME_FLAGS_LAST = SOF_TXTIME_REPORT_ERRORS,
    SOF_TXTIME_FLAGS_MASK = (SOF_TXTIME_FLAGS_LAST - 1) |
                             SOF_TXTIME_FLAGS_LAST
};

/* Publisher thread routine for ETF */
void *publisherETF(void *arg);
/* Subscriber thread routine */
void *subscriber(void *arg);
/* User application thread routine */
void *userApplicationPubSub(void *arg);
/* For adding nodes in the server information model */
static void addServerNodes(UA_Server *server);
/* For deleting the nodes created */
static void removeServerNodes(UA_Server *server);
/* To create multi-threads */
static pthread_t threadCreation(UA_Int16 threadPriority, size_t coreAffinity, void *(*thread) (void *),
                                char *applicationName, void *serverConfig);

/* Stop signal */
static void stopHandler(int sign) {
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "received ctrl-c");
    running = UA_FALSE;
}

/**
 * **Nanosecond field handling**
 *
 * Nanosecond field in timespec is checked for overflowing and one second
 * is added to seconds field and nanosecond field is set to zero
*/

static void nanoSecondFieldConversion(struct timespec *timeSpecValue) {
    /* Check if ns field is greater than '1 ns less than 1sec' */
    while (timeSpecValue->tv_nsec > (SECONDS -1)) {
        /* Move to next second and remove it from ns field */
        timeSpecValue->tv_sec  += SECONDS_INCREMENT;
        timeSpecValue->tv_nsec -= SECONDS;
    }

}

#if defined(SUBSCRIBER)
static void
addPubSubConnectionSubscriber(UA_Server *server, UA_NetworkAddressUrlDataType *networkAddressUrlSubscriber){
    UA_StatusCode    retval                                 = UA_STATUSCODE_GOOD;
    /* Details about the connection configuration and handling are located
     * in the pubsub connection tutorial */
    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name                                   = UA_STRING("Subscriber Connection");
    connectionConfig.enabled                                = UA_TRUE;
#ifdef UA_ENABLE_PUBSUB_ETH_UADP_XDP
    /* Connection options are given as Key/Value Pairs. */
    UA_KeyValuePair connectionOptions[2];
    connectionOptions[0].key                  = UA_QUALIFIEDNAME(0, "xdpflag");
    UA_UInt32 flags                           = XDP_FLAG;
    UA_Variant_setScalar(&connectionOptions[0].value, &flags, &UA_TYPES[UA_TYPES_UINT32]);
    connectionOptions[1].key                  = UA_QUALIFIEDNAME(0, "hwreceivequeue");
    UA_UInt32 rxqueue                         = RECEIVE_QUEUE;
    UA_Variant_setScalar(&connectionOptions[1].value, &rxqueue, &UA_TYPES[UA_TYPES_UINT32]);
    connectionConfig.connectionProperties     = connectionOptions;
    connectionConfig.connectionPropertiesSize = 2;
#endif
    UA_NetworkAddressUrlDataType networkAddressUrlsubscribe = *networkAddressUrlSubscriber;
    connectionConfig.transportProfileUri                    = UA_STRING(ETH_TRANSPORT_PROFILE);
    UA_Variant_setScalar(&connectionConfig.address, &networkAddressUrlsubscribe, &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric                    = UA_UInt32_random();
    retval |= UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdentSubscriber);
    if (retval == UA_STATUSCODE_GOOD)
         UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER,"The PubSub Connection was created successfully!");
}

/* Add ReaderGroup to the created connection */
static void
addReaderGroup(UA_Server *server) {
    if (server == NULL) {
        return;
    }

    UA_ReaderGroupConfig     readerGroupConfig;
    memset (&readerGroupConfig, 0, sizeof(UA_ReaderGroupConfig));
    readerGroupConfig.name   = UA_STRING("ReaderGroup1");
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    readerGroupConfig.rtLevel = UA_PUBSUB_RT_FIXED_SIZE;
#endif
    UA_Server_addReaderGroup(server, connectionIdentSubscriber, &readerGroupConfig,
                             &readerGroupIdentifier);
}

/* Add DataSetReader to the ReaderGroup */
static void
addDataSetReader(UA_Server *server) {
    UA_Int32 iterator = 0;
    if (server == NULL) {
        return;
    }

    memset (&readerConfig, 0, sizeof(UA_DataSetReaderConfig));
    readerConfig.name                 = UA_STRING("DataSet Reader 1");
    UA_UInt16 publisherIdentifier     = PUBLISHER_ID_SUB;
    readerConfig.publisherId.type     = &UA_TYPES[UA_TYPES_UINT16];
    readerConfig.publisherId.data     = &publisherIdentifier;
    readerConfig.writerGroupId        = WRITER_GROUP_ID_SUB;
    readerConfig.dataSetWriterId      = DATA_SET_WRITER_ID_SUB;

    readerConfig.messageSettings.encoding = UA_EXTENSIONOBJECT_DECODED;
    readerConfig.messageSettings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPDATASETREADERMESSAGEDATATYPE];
    UA_UadpDataSetReaderMessageDataType *dataSetReaderMessage = UA_UadpDataSetReaderMessageDataType_new();
    dataSetReaderMessage->networkMessageContentMask           = (UA_UadpNetworkMessageContentMask)(UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
                                                                 (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
                                                                 (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
                                                                 (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
    readerConfig.messageSettings.content.decoded.data = dataSetReaderMessage;

    /* Setting up Meta data configuration in DataSetReader */
    UA_DataSetMetaDataType *pMetaData = &readerConfig.dataSetMetaData;
    /* FilltestMetadata function in subscriber implementation */
    UA_DataSetMetaDataType_init(pMetaData);
    pMetaData->name                   = UA_STRING ("DataSet Test");
    /* Static definition of number of fields size to 1 to create one
       targetVariable */
    pMetaData->fieldsSize             = REPEATED_NODECOUNTS + 1;
    pMetaData->fields                 = (UA_FieldMetaData*)UA_Array_new (pMetaData->fieldsSize,
                                                                         &UA_TYPES[UA_TYPES_FIELDMETADATA]);

    for (iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_FieldMetaData_init (&pMetaData->fields[iterator]);
        UA_NodeId_copy (&UA_TYPES[UA_TYPES_UINT64].typeId,
                        &pMetaData->fields[iterator].dataType);
        pMetaData->fields[iterator].builtInType  = UA_NS0ID_UINT64;
        pMetaData->fields[iterator].valueRank    = -1; /* scalar */
    }

    /* Unsigned Integer DataType */
    UA_FieldMetaData_init (&pMetaData->fields[iterator]);
    UA_NodeId_copy (&UA_TYPES[UA_TYPES_UINT64].typeId,
                    &pMetaData->fields[iterator].dataType);
    pMetaData->fields[iterator].builtInType  = UA_NS0ID_UINT64;
    pMetaData->fields[iterator].valueRank    = -1; /* scalar */

    /* Setting up Meta data configuration in DataSetReader */
    UA_Server_addDataSetReader(server, readerGroupIdentifier, &readerConfig,
                               &readerIdentifier);
    UA_UadpDataSetReaderMessageDataType_delete(dataSetReaderMessage);
}

/* Set SubscribedDataSet type to TargetVariables data type
 * Add SubscriberCounter variable to the DataSetReader */
static void addSubscribedVariables (UA_Server *server, UA_NodeId dataSetReaderId) {
    UA_Int32 iterator = 0;
    if (server == NULL) {
        return;
    }

    UA_TargetVariablesDataType targetVars;
    targetVars.targetVariablesSize = REPEATED_NODECOUNTS + 1;
    targetVars.targetVariables     = (UA_FieldTargetDataType *)
                                      UA_calloc(targetVars.targetVariablesSize,
                                      sizeof(UA_FieldTargetDataType));
    /* For creating Targetvariable */
    for (iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_FieldTargetDataType_init(&targetVars.targetVariables[iterator]);
        targetVars.targetVariables[iterator].attributeId  = UA_ATTRIBUTEID_VALUE;
        targetVars.targetVariables[iterator].targetNodeId = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+50000);
    }

    UA_FieldTargetDataType_init(&targetVars.targetVariables[iterator]);
    targetVars.targetVariables[iterator].attributeId  = UA_ATTRIBUTEID_VALUE;
    targetVars.targetVariables[iterator].targetNodeId = subNodeID;
    UA_Server_DataSetReader_createTargetVariables(server, dataSetReaderId, &targetVars);

    UA_TargetVariablesDataType_clear(&targetVars);
    UA_free(readerConfig.dataSetMetaData.fields);
}

#endif

/* Add a callback for cyclic repetition */
UA_StatusCode
UA_PubSubManager_addRepeatedCallback(UA_Server *server, UA_ServerCallback callback,
                                     void *data, UA_Double interval_ms, UA_UInt64 *callbackId) {
    /* Initialize arguments required for the thread to run */
    threadArg *threadArguments = (threadArg *) UA_malloc(sizeof(threadArg));

    /* Pass the value required for the threads */
    threadArguments->server      = server;
    threadArguments->data        = data;
    threadArguments->callback    = callback;
    threadArguments->interval_ms = interval_ms;
    threadArguments->callbackId  = callbackId;

    /* Check the writer group identifier and create the thread accordingly */
    UA_WriterGroup *tmpWriter  = (UA_WriterGroup *) data;
    if(UA_NodeId_equal(&tmpWriter->identifier, &writerGroupIdent)) {
#if defined(PUBLISHER)
        /* Create the publisher thread with the required priority and core affinity */
        char threadNamePub[10] = "Publisher";
        pubthreadID            = threadCreation(PUB_SCHED_PRIORITY, CORE_TWO, publisherETF, threadNamePub, threadArguments);
#endif
    }
    else {
#if defined(SUBSCRIBER)
        /* Create the subscriber thread with the required priority and core affinity */
        char threadNameSub[11] = "Subscriber";
        subthreadID            = threadCreation(SUB_SCHED_PRIORITY, CORE_TWO, subscriber, threadNameSub, threadArguments);
#endif
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_PubSubManager_changeRepeatedCallbackInterval(UA_Server *server, UA_UInt64 callbackId,
                                                UA_Double interval_ms) {
    /* Callback interval need not be modified as it is thread based implementation.
     * The thread uses nanosleep for calculating cycle time and modification in
     * nanosleep value changes cycle time */
    return UA_STATUSCODE_GOOD;
}

/* Remove the callback added for cyclic repetition */
void
UA_PubSubManager_removeRepeatedPubSubCallback(UA_Server *server, UA_UInt64 callbackId) {
    /* TODO Thread exit functions using pthread join and exit */
}

#if defined(PUBLISHER)
/**
 * **PubSub connection handling**
 *
 * Create a new ConnectionConfig. The addPubSubConnection function takes the
 * config and creates a new connection. The Connection identifier is
 * copied to the NodeId parameter.
 */
static void
addPubSubConnection(UA_Server *server, UA_NetworkAddressUrlDataType *networkAddressUrlPub){
    /* Details about the connection configuration and handling are located
     * in the pubsub connection tutorial */
    UA_PubSubConnectionConfig connectionConfig;
    memset(&connectionConfig, 0, sizeof(connectionConfig));
    connectionConfig.name                                   = UA_STRING("Publisher Connection");
    connectionConfig.enabled                                = UA_TRUE;
    UA_NetworkAddressUrlDataType networkAddressUrl          = *networkAddressUrlPub;
    connectionConfig.transportProfileUri                    = UA_STRING(ETH_TRANSPORT_PROFILE);
    UA_Variant_setScalar(&connectionConfig.address, &networkAddressUrl,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);
    connectionConfig.publisherId.numeric                    = PUBLISHER_ID;
    /* ETF configuration settings */
    connectionConfig.etfConfiguration.socketPriority        = SOCKET_PRIORITY;
    connectionConfig.etfConfiguration.sotxtimeEnabled       = UA_TRUE;
    UA_Server_addPubSubConnection(server, &connectionConfig, &connectionIdent);
}

/**
 * **PublishedDataSet handling**
 *
 * Details about the connection configuration and handling are located
 * in the pubsub connection tutorial
 */
static void
addPublishedDataSet(UA_Server *server) {
    UA_PublishedDataSetConfig publishedDataSetConfig;
    memset(&publishedDataSetConfig, 0, sizeof(UA_PublishedDataSetConfig));
    publishedDataSetConfig.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    publishedDataSetConfig.name                 = UA_STRING("Demo PDS");
    UA_Server_addPublishedDataSet(server, &publishedDataSetConfig, &publishedDataSetIdent);
}

/**
 * **DataSetField handling**
 *
 * The DataSetField (DSF) is part of the PDS and describes exactly one
 * published field.
 */
static void
addDataSetField(UA_Server *server) {
    /* Add a field to the previous created PublishedDataSet */
    UA_NodeId dataSetFieldIdentRepeated;
    UA_DataSetFieldConfig dataSetFieldConfig;
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    staticValueSource = UA_DataValue_new();
#endif
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
    {
       memset(&dataSetFieldConfig, 0, sizeof(UA_DataSetFieldConfig));
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
       UA_UInt64 *repeatValue        = UA_UInt64_new();
       repeatedCounterData[iterator] = repeatValue;
       UA_Variant_setScalar(&staticValueSource->value, repeatValue, &UA_TYPES[UA_TYPES_UINT64]);
       dataSetFieldConfig.field.variable.rtValueSource.rtFieldSourceEnabled = UA_TRUE;
       dataSetFieldConfig.field.variable.rtValueSource.staticValueSource = &staticValueSource;
#else
       repeatedCounterData[iterator] = UA_UInt64_new();
       dataSetFieldConfig.dataSetFieldType                                   = UA_PUBSUB_DATASETFIELD_VARIABLE;
       dataSetFieldConfig.field.variable.fieldNameAlias                      = UA_STRING("Repeated Counter Variable");
       dataSetFieldConfig.field.variable.promotedField                       = UA_FALSE;
       dataSetFieldConfig.field.variable.publishParameters.publishedVariable = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+10000);
       dataSetFieldConfig.field.variable.publishParameters.attributeId       = UA_ATTRIBUTEID_VALUE;
#endif
       UA_Server_addDataSetField(server, publishedDataSetIdent, &dataSetFieldConfig, &dataSetFieldIdentRepeated);
   }

    UA_NodeId dataSetFieldIdent;
    UA_DataSetFieldConfig counterValue;
    memset(&counterValue, 0, sizeof(UA_DataSetFieldConfig));
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    UA_UInt64 *countValue = UA_UInt64_new();
    pubCounterData = countValue;
    UA_Variant_setScalar(&staticValueSource->value, countValue, &UA_TYPES[UA_TYPES_UINT64]);
    counterValue.field.variable.rtValueSource.rtFieldSourceEnabled = UA_TRUE;
    counterValue.field.variable.rtValueSource.staticValueSource = &staticValueSource;
#else
    pubCounterData = UA_UInt64_new();
    counterValue.dataSetFieldType                                   = UA_PUBSUB_DATASETFIELD_VARIABLE;
    counterValue.field.variable.fieldNameAlias                      = UA_STRING("Counter Variable");
    counterValue.field.variable.promotedField                       = UA_FALSE;
    counterValue.field.variable.publishParameters.publishedVariable = pubNodeID;
    counterValue.field.variable.publishParameters.attributeId       = UA_ATTRIBUTEID_VALUE;
#endif
    UA_Server_addDataSetField(server, publishedDataSetIdent, &counterValue, &dataSetFieldIdent);

}

/**
 * **WriterGroup handling**
 *
 * The WriterGroup (WG) is part of the connection and contains the primary
 * configuration parameters for the message creation.
 */
static void
addWriterGroup(UA_Server *server) {
    UA_WriterGroupConfig writerGroupConfig;
    memset(&writerGroupConfig, 0, sizeof(UA_WriterGroupConfig));
    writerGroupConfig.name               = UA_STRING("Demo WriterGroup");
    writerGroupConfig.publishingInterval = CYCLE_TIME;
    writerGroupConfig.enabled            = UA_FALSE;
    writerGroupConfig.encodingMimeType   = UA_PUBSUB_ENCODING_UADP;
    writerGroupConfig.writerGroupId      = WRITER_GROUP_ID;
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    writerGroupConfig.rtLevel            = UA_PUBSUB_RT_FIXED_SIZE;
#endif
    writerGroupConfig.messageSettings.encoding             = UA_EXTENSIONOBJECT_DECODED;
    writerGroupConfig.messageSettings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
    /* The configuration flags for the messages are encapsulated inside the
     * message- and transport settings extension objects. These extension
     * objects are defined by the standard. e.g.
     * UadpWriterGroupMessageDataType */
    UA_UadpWriterGroupMessageDataType *writerGroupMessage  = UA_UadpWriterGroupMessageDataType_new();
    /* Change message settings of writerGroup to send PublisherId,
     * WriterGroupId in GroupHeader and DataSetWriterId in PayloadHeader
     * of NetworkMessage */
    writerGroupMessage->networkMessageContentMask          = (UA_UadpNetworkMessageContentMask)(UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
                                                              (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
                                                              (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
                                                              (UA_UadpNetworkMessageContentMask)UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
    writerGroupConfig.messageSettings.content.decoded.data = writerGroupMessage;
    UA_Server_addWriterGroup(server, connectionIdent, &writerGroupConfig, &writerGroupIdent);
    UA_Server_setWriterGroupOperational(server, writerGroupIdent);
    UA_UadpWriterGroupMessageDataType_delete(writerGroupMessage);
}

/**
 * **DataSetWriter handling**
 *
 * A DataSetWriter (DSW) is the glue between the WG and the PDS. The DSW is
 * linked to exactly one PDS and contains additional informations for the
 * message generation.
 */
static void
addDataSetWriter(UA_Server *server) {
    UA_NodeId dataSetWriterIdent;
    UA_DataSetWriterConfig dataSetWriterConfig;
    memset(&dataSetWriterConfig, 0, sizeof(UA_DataSetWriterConfig));
    dataSetWriterConfig.name            = UA_STRING("Demo DataSetWriter");
    dataSetWriterConfig.dataSetWriterId = DATA_SET_WRITER_ID;
    dataSetWriterConfig.keyFrameCount   = 10;
    UA_Server_addDataSetWriter(server, writerGroupIdent, publishedDataSetIdent,
                               &dataSetWriterConfig, &dataSetWriterIdent);
}

#if defined(UPDATE_MEASUREMENTS)
/**
 * **Published data handling**
 *
 * The published data is updated in the array using this function
 */
#if defined(PUBLISHER)
static void
updateMeasurementsPublisher(struct timespec start_time,
                            UA_UInt64 counterValue) {
    publishTimestamp[measurementsPublisher]        = start_time;
    publishCounterValue[measurementsPublisher]     = counterValue;
    measurementsPublisher++;
}
#endif
#if defined(SUBSCRIBER)
/**
 * Subscribed data handling**
 * The subscribed data is updated in the array using this function Subscribed data handling**
 */
static void
updateMeasurementsSubscriber(struct timespec receive_time, UA_UInt64 counterValue) {
    subscribeTimestamp[measurementsSubscriber]     = receive_time;
    subscribeCounterValue[measurementsSubscriber]  = counterValue;
    measurementsSubscriber++;
}
#endif
#endif

/**
 * **Publisher thread routine**
 *
 * The publisherETF function is the routine used by the publisher thread.
 * This routine publishes the data at a cycle time of 250us.
 */
void *publisherETF(void *arg) {
    struct timespec   nextnanosleeptime;
    UA_ServerCallback pubCallback;
    UA_Server*        server;
    UA_WriterGroup*   currentWriterGroup;
    UA_UInt64         interval_ns;
    UA_UInt64         transmission_time;

    /* Initialise value for nextnanosleeptime timespec */
    nextnanosleeptime.tv_nsec                      = 0;

    threadArg *threadArgumentsPublisher = (threadArg *)arg;
    server                              = threadArgumentsPublisher->server;
    pubCallback                         = threadArgumentsPublisher->callback;
    currentWriterGroup                  = (UA_WriterGroup *)threadArgumentsPublisher->data;
    interval_ns                         = (threadArgumentsPublisher->interval_ms * MILLI_SECONDS);

    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptime);
    /* Variable to nano Sleep until 1ms before a 1 second boundary */
    nextnanosleeptime.tv_sec                      += SECONDS_SLEEP;
    nextnanosleeptime.tv_nsec                      = NANO_SECONDS_SLEEP_PUB;
    nanoSecondFieldConversion(&nextnanosleeptime);

    /* Define Ethernet ETF transport settings */
    UA_EthernetETFWriterGroupTransportDataType ethernetETFtransportSettings;
    memset(&ethernetETFtransportSettings, 0, sizeof(UA_EthernetETFWriterGroupTransportDataType));
    /* TODO: Txtime enable shall be configured based on connectionConfig.etfConfiguration.sotxtimeEnabled parameter */
    ethernetETFtransportSettings.txtime_enabled    = UA_TRUE;
    ethernetETFtransportSettings.transmission_time = 0;

    /* Encapsulate ETF config in transportSettings */
    UA_ExtensionObject transportSettings;
    memset(&transportSettings, 0, sizeof(UA_ExtensionObject));
    /* TODO: transportSettings encoding and type to be defined */
    transportSettings.content.decoded.data       = &ethernetETFtransportSettings;
    currentWriterGroup->config.transportSettings = transportSettings;
    UA_UInt64 roundOffCycleTime                  = (CYCLE_TIME * MILLI_SECONDS) - NANO_SECONDS_SLEEP_PUB;

    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptime, NULL);
        transmission_time                              = ((UA_UInt64)nextnanosleeptime.tv_sec * SECONDS + (UA_UInt64)nextnanosleeptime.tv_nsec) + roundOffCycleTime + QBV_OFFSET;
        ethernetETFtransportSettings.transmission_time = transmission_time;
        pubCallback(server, currentWriterGroup);
        nextnanosleeptime.tv_nsec                     += interval_ns;
        nanoSecondFieldConversion(&nextnanosleeptime);
    }

    UA_free(threadArgumentsPublisher);

    return (void*)NULL;
}
#endif

#if defined(SUBSCRIBER)
/**
 * **Subscriber thread routine**
 *
 * The subscriber function is the routine used by the subscriber thread.
 */

void *subscriber(void *arg) {
    UA_Server*        server;
    UA_ReaderGroup*   currentReaderGroup;
    UA_ServerCallback subCallback;
    struct timespec   nextnanosleeptimeSub;

    threadArg *threadArgumentsSubscriber = (threadArg *)arg;
    server                               = threadArgumentsSubscriber->server;
    subCallback                          = threadArgumentsSubscriber->callback;
    currentReaderGroup                   = (UA_ReaderGroup *)threadArgumentsSubscriber->data;

    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptimeSub);
    /* Variable to nano Sleep until 1ms before a 1 second boundary */
    nextnanosleeptimeSub.tv_sec         += SECONDS_SLEEP;
    nextnanosleeptimeSub.tv_nsec         = NANO_SECONDS_SLEEP_SUB;
    nanoSecondFieldConversion(&nextnanosleeptimeSub);
    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptimeSub, NULL);
        /* Read subscribed data from the SubscriberCounter variable */
        subCallback(server, currentReaderGroup);
        nextnanosleeptimeSub.tv_nsec += (CYCLE_TIME * MILLI_SECONDS);
        nanoSecondFieldConversion(&nextnanosleeptimeSub);
    }

    UA_free(threadArgumentsSubscriber);

    return (void*)NULL;
}
#endif

#if defined(PUBLISHER) || defined(SUBSCRIBER)
/**
 * **UserApplication thread routine**
 *
 */
void *userApplicationPubSub(void *arg) {
    UA_Server* server;
    UA_UInt64  repeatedCounterValue = 10;
    struct timespec nextnanosleeptimeUserApplication;
    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptimeUserApplication);
    /* Variable to nano Sleep until 1ms before a 1 second boundary */
    nextnanosleeptimeUserApplication.tv_sec                      += SECONDS_SLEEP;
    nextnanosleeptimeUserApplication.tv_nsec                      = NANO_SECONDS_SLEEP_USER_APPLICATION;
    nanoSecondFieldConversion(&nextnanosleeptimeUserApplication);
    *pubCounterData      = 0;
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
    {
        *repeatedCounterData[iterator] = repeatedCounterValue;
    }
    serverConfigStruct *serverConfig = (serverConfigStruct*)arg;
    server = serverConfig->ServerRun;
    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptimeUserApplication, NULL);
#if defined(PUBLISHER)
        *pubCounterData      = *pubCounterData + 1;
        for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
        {
            *repeatedCounterData[iterator] = *repeatedCounterData[iterator] + 1;
        }
        clock_gettime(CLOCKID, &dataModificationTime);
#ifndef PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
        UA_Variant pubCounter;
        UA_Variant_init(&pubCounter);
        UA_Variant_setScalar(&pubCounter, pubCounterData, &UA_TYPES[UA_TYPES_UINT64]);
        UA_NodeId currentNodeId = UA_NODEID_STRING(1, "PublisherCounter");
        UA_Server_writeValue(server, currentNodeId, pubCounter);
        for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
        {
            UA_Variant rpCounter;
            UA_Variant_init(&rpCounter);
            UA_Variant_setScalar(&rpCounter, repeatedCounterData[iterator], &UA_TYPES[UA_TYPES_UINT64]);
            UA_Server_writeValue(server, UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+10000), rpCounter);
        }
#endif
#endif
#if defined(SUBSCRIBER)
        const UA_NodeId nodeid  = UA_NODEID_STRING(1,"SubscriberCounter");
        UA_Variant subCounter;
        UA_Variant_init(&subCounter);
        UA_Server_readValue(server, nodeid, &subCounter);
        subCounterData          = *(UA_UInt64 *)subCounter.data;
        clock_gettime(CLOCKID, &dataReceiveTime);
        UA_Variant_clear(&subCounter);
#if defined(UPDATE_MEASUREMENTS)
        updateMeasurementsPublisher(dataModificationTime, *pubCounterData);
        if (subCounterData > 0)
            updateMeasurementsSubscriber(dataReceiveTime, subCounterData);
#endif
#endif
        nextnanosleeptimeUserApplication.tv_nsec += (CYCLE_TIME * MILLI_SECONDS);
        nanoSecondFieldConversion(&nextnanosleeptimeUserApplication);
    }

#ifndef PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    UA_free(pubCounterData);
    for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
    {
        UA_free(repeatedCounterData[iterator]);
    }
#endif

    return (void*)NULL;
}
#endif
/**
 * **Deletion of nodes**
 *
 * The removeServerNodes function is used to delete the publisher and subscriber
 * nodes.
 */
static void removeServerNodes(UA_Server *server) {
#ifndef PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    /* Delete the Publisher Counter Node*/
    UA_Server_deleteNode(server, pubNodeID, UA_TRUE);
    UA_NodeId_clear(&pubNodeID);
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_Server_deleteNode(server, pubRepeatedCountNodeID, UA_TRUE);
        UA_NodeId_clear(&pubRepeatedCountNodeID);
    }
#endif
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_Server_deleteNode(server, subRepeatedCountNodeID, UA_TRUE);
        UA_NodeId_clear(&subRepeatedCountNodeID);
    }
}

static pthread_t threadCreation(UA_Int16 threadPriority, size_t coreAffinity, void *(*thread) (void *), char *applicationName, void *serverConfig){

    /* Core affinity set */
    cpu_set_t           cpuset;
    pthread_t           threadID;
    struct sched_param  schedParam;
    UA_Int32         returnValue         = 0;
    UA_Int32         errorSetAffinity    = 0;
    /* Return the ID for thread */
    threadID = pthread_self();
    schedParam.sched_priority = threadPriority;
    returnValue = pthread_setschedparam(threadID, SCHED_FIFO, &schedParam);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"pthread_setschedparam: failed\n");
        exit(1);
    }
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,\
                "\npthread_setschedparam:%s Thread priority is %d \n", \
                applicationName, schedParam.sched_priority);
    CPU_ZERO(&cpuset);
    CPU_SET(coreAffinity, &cpuset);
    errorSetAffinity = pthread_setaffinity_np(threadID, sizeof(cpu_set_t), &cpuset);
    if (errorSetAffinity) {
        fprintf(stderr, "pthread_setaffinity_np: %s\n", strerror(errorSetAffinity));
        exit(1);
    }

    returnValue = pthread_create(&threadID, NULL, thread, serverConfig);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,":%s Cannot create thread\n", applicationName);
    }

    if (CPU_ISSET(coreAffinity, &cpuset)) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"%s CPU CORE: %ld\n", applicationName, coreAffinity);
    }

   return threadID;

}
/**
 * **Creation of nodes**
 *
 * The addServerNodes function is used to create the publisher and subscriber
 * nodes.
 */
static void addServerNodes(UA_Server *server) {
    UA_NodeId objectId;
    UA_NodeId newNodeId;
    UA_ObjectAttributes object           = UA_ObjectAttributes_default;
    object.displayName                   = UA_LOCALIZEDTEXT("en-US", "Counter Object");
    UA_Server_addObjectNode(server, UA_NODEID_NULL,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, "Counter Object"), UA_NODEID_NULL,
                            object, NULL, &objectId);
#ifndef PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    UA_VariableAttributes publisherAttr  = UA_VariableAttributes_default;
    UA_UInt64 publishValue               = 0;
    publisherAttr.accessLevel            = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Variant_setScalar(&publisherAttr.value, &publishValue, &UA_TYPES[UA_TYPES_UINT64]);
    publisherAttr.displayName            = UA_LOCALIZEDTEXT("en-US", "Publisher Counter");
    newNodeId                            = UA_NODEID_STRING(1, "PublisherCounter");
    UA_Server_addVariableNode(server, newNodeId, objectId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Publisher Counter"),
                              UA_NODEID_NULL, publisherAttr, NULL, &pubNodeID);
#endif
    UA_VariableAttributes subscriberAttr = UA_VariableAttributes_default;
    UA_UInt64 subscribeValue             = 0;
    subscriberAttr.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Variant_setScalar(&subscriberAttr.value, &subscribeValue, &UA_TYPES[UA_TYPES_UINT64]);
    subscriberAttr.displayName           = UA_LOCALIZEDTEXT("en-US", "Subscriber Counter");
    newNodeId                            = UA_NODEID_STRING(1, "SubscriberCounter");
    UA_Server_addVariableNode(server, newNodeId, objectId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Subscriber Counter"),
                              UA_NODEID_NULL, subscriberAttr, NULL, &subNodeID);

#ifndef PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_VariableAttributes repeatedNodePub = UA_VariableAttributes_default;
        UA_UInt64 repeatedPublishValue        = 0;
        repeatedNodePub.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        UA_Variant_setScalar(&repeatedNodePub.value, &repeatedPublishValue, &UA_TYPES[UA_TYPES_UINT64]);
        repeatedNodePub.displayName           = UA_LOCALIZEDTEXT("en-US", "Publisher RepeatedCounter");
        newNodeId                             = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+10000);
        UA_Server_addVariableNode(server, newNodeId, objectId,
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                 UA_QUALIFIEDNAME(1, "Publisher RepeatedCounter"),
                                 UA_NODEID_NULL, repeatedNodePub, NULL, &pubRepeatedCountNodeID);
    }
#endif

    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_VariableAttributes repeatedNodeSub = UA_VariableAttributes_default;
        UA_UInt64 repeatedSubscribeValue;
        UA_Variant_setScalar(&repeatedNodeSub.value, &repeatedSubscribeValue, &UA_TYPES[UA_TYPES_UINT64]);
        repeatedNodeSub.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        repeatedNodeSub.displayName           = UA_LOCALIZEDTEXT("en-US", "Subscriber RepeatedCounter");
        newNodeId                             = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+50000);
        UA_Server_addVariableNode(server, newNodeId, objectId,
                                 UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                 UA_QUALIFIEDNAME(1, "Subscriber RepeatedCounter"),
                                 UA_NODEID_NULL, repeatedNodeSub, NULL, &subRepeatedCountNodeID);
    }

}

static void
usage(char *progname) {
    printf("usage: %s <ethernet_interface> \n", progname);
    printf("Provide the Interface parameter to run the application. Exiting \n");
 }

/**
 * **Main Server code**
 *
 * The main function contains publisher and subscriber threads running in
 * parallel.
 */
int main(int argc, char **argv) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    UA_Int32         returnValue         = 0;
    UA_StatusCode    retval              = UA_STATUSCODE_GOOD;
    UA_Server       *server              = UA_Server_new();
    UA_ServerConfig *config              = UA_Server_getConfig(server);
    pthread_t        userThreadID;
    UA_ServerConfig_setMinimal(config, PORT_NUMBER, NULL);
    UA_NetworkAddressUrlDataType networkAddressUrlPub;

#if defined(SUBSCRIBER)
    UA_NetworkAddressUrlDataType networkAddressUrlSub;
#endif
    if (argc == 1) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }

#if defined(PUBLISHER)
        networkAddressUrlPub.networkInterface = UA_STRING(argv[1]);
#endif
#if defined(SUBSCRIBER)
        networkAddressUrlSub.networkInterface = UA_STRING(argv[1]);
#endif

    }

    else {
#if defined(PUBLISHER)
        networkAddressUrlPub.url = UA_STRING(PUBLISHING_MAC_ADDRESS); /* MAC address of subscribing node*/
#endif
#if defined(SUBSCRIBER)
        networkAddressUrlSub.url = UA_STRING(SUBSCRIBING_MAC_ADDRESS); /* Self MAC address */
#endif
    }

#if defined(UPDATE_MEASUREMENTS)
#if defined(PUBLISHER)
    fpPublisher                   = fopen(filePublishedData, "w");
#endif
#if defined(SUBSCRIBER)
    fpSubscriber                  = fopen(fileSubscribedData, "w");
#endif
#endif

#if defined(PUBLISHER) && defined(SUBSCRIBER)
/* Details about the connection configuration and handling are located in the pubsub connection tutorial */
    config->pubsubTransportLayers = (UA_PubSubTransportLayer *)
                                    UA_malloc(2 * sizeof(UA_PubSubTransportLayer));
#else
    config->pubsubTransportLayers = (UA_PubSubTransportLayer *)
                                    UA_malloc(sizeof(UA_PubSubTransportLayer));
#endif
    if (!config->pubsubTransportLayers) {
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }

/* It is possible to use multiple PubSubTransportLayers on runtime.
 * The correct factory is selected on runtime by the standard defined
 * PubSub TransportProfileUri's.
*/

#if defined (PUBLISHER)
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernetETF();
    config->pubsubTransportLayersSize++;
#endif

    /* Create variable nodes for publisher and subscriber in address space */
    addServerNodes(server);

#if defined(PUBLISHER)
    addPubSubConnection(server, &networkAddressUrlPub);
    addPublishedDataSet(server);
    addDataSetField(server);
    addWriterGroup(server);
    addDataSetWriter(server);
    UA_Server_freezeWriterGroupConfiguration(server, writerGroupIdent);
#endif

#if defined (PUBLISHER) && defined(SUBSCRIBER)
#if defined (UA_ENABLE_PUBSUB_ETH_UADP_XDP)
    config->pubsubTransportLayers[1] = UA_PubSubTransportLayerEthernetXDP();
    config->pubsubTransportLayersSize++;
#else
    config->pubsubTransportLayers[1] = UA_PubSubTransportLayerEthernetETF();
    config->pubsubTransportLayersSize++;
#endif
#endif

#if defined(SUBSCRIBER) && !defined(PUBLISHER)
#if defined (UA_ENABLE_PUBSUB_ETH_UADP_XDP)
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernetXDP();
    config->pubsubTransportLayersSize++;
#else
    config->pubsubTransportLayers[0] = UA_PubSubTransportLayerEthernetETF();
    config->pubsubTransportLayersSize++;
#endif
#endif

#if defined(SUBSCRIBER)
    addPubSubConnectionSubscriber(server, &networkAddressUrlSub);
    addReaderGroup(server);
    addDataSetReader(server);
    addSubscribedVariables(server, readerIdentifier);
    UA_Server_freezeReaderGroupConfiguration(server, readerGroupIdentifier);
    UA_Server_setReaderGroupOperational(server, readerGroupIdentifier);
#endif
    serverConfigStruct *serverConfig;
    serverConfig            = (serverConfigStruct*)UA_malloc(sizeof(serverConfigStruct));
    serverConfig->ServerRun = server;

#if defined(PUBLISHER) || defined(SUBSCRIBER)
    char threadNameUserApplication[22] = "UserApplicationPubSub";
    userThreadID                       = threadCreation(USERAPPLICATION_SCHED_PRIORITY, CORE_THREE, userApplicationPubSub, threadNameUserApplication, serverConfig);
#endif
    retval |= UA_Server_run(server, &running);

    UA_Server_unfreezeReaderGroupConfiguration(server, readerGroupIdentifier);
#if defined(PUBLISHER)
    returnValue = pthread_join(pubthreadID, NULL);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"\nPthread Join Failed for publisher thread:%d\n", returnValue);
    }
#endif
#if defined(SUBSCRIBER)
    returnValue = pthread_join(subthreadID, NULL);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"\nPthread Join Failed for subscriber thread:%d\n", returnValue);
    }
#endif
#if defined(PUBLISHER) || defined(SUBSCRIBER)
    returnValue = pthread_join(userThreadID, NULL);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"\nPthread Join Failed for User thread:%d\n", returnValue);
    }
#endif

#if defined(PUBLISHER)
#if defined(UPDATE_MEASUREMENTS)
    /* Write the published data in the publisher_T1.csv file */
   size_t pubLoopVariable               = 0;
   for (pubLoopVariable = 0; pubLoopVariable < measurementsPublisher;
        pubLoopVariable++) {
        fprintf(fpPublisher, "%ld,%ld.%09ld\n",
                publishCounterValue[pubLoopVariable],
                publishTimestamp[pubLoopVariable].tv_sec,
                publishTimestamp[pubLoopVariable].tv_nsec);
    }
#endif
#endif
#if defined(SUBSCRIBER)
#if defined(UPDATE_MEASUREMENTS)
    /* Write the subscribed data in the subscriber_T8.csv file */
    size_t subLoopVariable               = 0;
    for (subLoopVariable = 0; subLoopVariable < measurementsSubscriber;
         subLoopVariable++) {
        fprintf(fpSubscriber, "%ld,%ld.%09ld\n",
                subscribeCounterValue[subLoopVariable],
                subscribeTimestamp[subLoopVariable].tv_sec,
                subscribeTimestamp[subLoopVariable].tv_nsec);
    }
#endif
#endif
#if defined(PUBLISHER) || defined(SUBSCRIBER)
    removeServerNodes(server);
    UA_Server_delete(server);
    UA_free(serverConfig);
#endif
#if defined(PUBLISHER)
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
     UA_DataValue_delete(staticValueSource);
#endif
#if defined(UPDATE_MEASUREMENTS)
    fclose(fpPublisher);
#endif
#endif

#if defined(SUBSCRIBER)
#if defined(UPDATE_MEASUREMENTS)
    fclose(fpSubscriber);
#endif
#endif
    return (int)retval;
}
