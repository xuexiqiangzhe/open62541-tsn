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

#ifdef UA_ENABLE_PUBSUB_ETH_UADP_XDP
#include <linux/if_link.h>
#include <open62541/plugin/pubsub_ethernet_xdp.h>
#endif

#include "ua_pubsub.h"

UA_NodeId readerGroupIdentifier;
UA_NodeId readerIdentifier;

UA_DataSetReaderConfig readerConfig;

/*to find load of each thread
 * ps -L -o pid,pri,%cpu -C pubsub_TSN_loopback */

/* Configurable Parameters */
/* These defines enables the publisher and subscriber of the OPCUA stack */
/* To run only publisher, enable PUBLISHER define alone (comment SUBSCRIBER) */
//#define             PUBLISHER
/* To run only subscriber, enable SUBSCRIBER define alone (comment PUBLISHER) */
#define             SUBSCRIBER
#define             UPDATE_MEASUREMENTS
/* Cycle time in milliseconds */
#define             CYCLE_TIME                            0.25
/* Qbv offset */
#define             QBV_OFFSET                            25 * 1000
#define             SOCKET_PRIORITY                       3
#if defined(PUBLISHER)
#define             PUBLISHER_ID                          2235
#define             WRITER_GROUP_ID                       100
#define             DATA_SET_WRITER_ID                     62541
#define             PUBLISHING_MAC_ADDRESS                "opc.eth://01-00-5E-00-00-01"
#endif
#if defined(SUBSCRIBER)
#define             PUBLISHER_ID_SUB                     2234
#define             WRITER_GROUP_ID_SUB                  101
#define             DATA_SET_WRITER_ID_SUB               62541
#define             SUBSCRIBING_MAC_ADDRESS              "opc.eth://01-00-5E-7F-00-01"
#endif
#define             REPEATED_NODECOUNTS               7//YXHGL
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
/* Priority of Publisher, subscriber, User application and server are kept */
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
//UA_NodeId           subNodeID;//YXHGL
UA_NodeId           pubRepeatedCountNodeID;
//UA_NodeId           subRepeatedCountNodeID;//YXHGL
/* Variables for counter data handling in address space */
UA_UInt64           *pubCounterData;
UA_UInt64           *repeatedCounterData[REPEATED_NODECOUNTS];
UA_DataValue        *staticValueSource;

UA_UInt64           subCounterData         = 0;
/*信息模型*/
UA_NodeId RefrigerationId; 
UA_NodeId numberNode ;
UA_NodeId rtpostionNode ;
UA_NodeId settingTSNNode ;
UA_NodeId CycletimeTSNNode;
UA_NodeId QbvTSNNode ;
UA_NodeId statusNode;
UA_NodeId stopcurrentlyNode;
UA_NodeId resetNode;
UA_NodeId backtozeroNode;
UA_NodeId moveNode ;
#if defined(PUBLISHER)
#if defined(UPDATE_MEASUREMENTS)
/* File to store the data and timestamps for different traffic */
FILE               *fpPublisher;
char               *filePublishedData      = "publisher_T5.csv";
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
char               *fileSubscribedData     = "subscriber_T4.csv";
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
//void *userApplicationPubSub(void *arg);
/* For adding nodes in the server information model */
//static void addServerNodes(UA_Server *server);
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
    if(server == NULL) {
        return;
    }

    UA_ReaderGroupConfig readerGroupConfig;
    memset (&readerGroupConfig, 0, sizeof(UA_ReaderGroupConfig));
    readerGroupConfig.name = UA_STRING("ReaderGroup");
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
    if(server == NULL) {
        return;
    }

    memset (&readerConfig, 0, sizeof(UA_DataSetReaderConfig));
    readerConfig.name                 = UA_STRING("DataSet Reader");
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
    UA_DataSetMetaDataType_init (pMetaData);
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
 * Add subscribedvariables to the DataSetReader */
static void addSubscribedVariables (UA_Server *server, UA_NodeId dataSetReaderId) {
    UA_Int32 iterator = 0;
    if(server == NULL) {
        return;
    }

    UA_TargetVariablesDataType targetVars;
    targetVars.targetVariablesSize = REPEATED_NODECOUNTS + 1;
    targetVars.targetVariables     = (UA_FieldTargetDataType *)
                                      UA_calloc(targetVars.targetVariablesSize,
                                      sizeof(UA_FieldTargetDataType));
    /* For creating Targetvariable */
    for (iterator = 0; iterator < REPEATED_NODECOUNTS+1; iterator++)
    {
        UA_FieldTargetDataType_init(&targetVars.targetVariables[iterator]);
        targetVars.targetVariables[iterator].attributeId  = UA_ATTRIBUTEID_VALUE;
        switch (iterator)//YXHGL
        {
        case 0:
        targetVars.targetVariables[iterator].targetNodeId = rtpostionNode;
            break;

        case 1:
            targetVars.targetVariables[iterator].targetNodeId = CycletimeTSNNode;
            break;

            case 2:
            targetVars.targetVariables[iterator].targetNodeId = QbvTSNNode;
            break;

            case 3:
             targetVars.targetVariables[iterator].targetNodeId = statusNode;
            break;

            case 4:
           targetVars.targetVariables[iterator].targetNodeId = stopcurrentlyNode;
            break;

            case 5:
             targetVars.targetVariables[iterator].targetNodeId = resetNode;
            break;

            case 6:
            targetVars.targetVariables[iterator].targetNodeId = backtozeroNode;
            break;

            case 7:
            targetVars.targetVariables[iterator].targetNodeId = moveNode;
            break;
        }
           printf("%d\t,Target Node ID:%d\n",iterator, targetVars.targetVariables[iterator].targetNodeId.identifier.numeric);

    }

    /*UA_FieldTargetDataType_init(&targetVars.targetVariables[iterator]);
    targetVars.targetVariables[iterator].attributeId  = UA_ATTRIBUTEID_VALUE;
    targetVars.targetVariables[iterator].targetNodeId = subNodeID;*/
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
    UA_WriterGroup *tmpWriter = (UA_WriterGroup *) data;
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
 * config and create a new connection. The Connection identifier is
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
 * **PublishedDataset handling**
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
    UA_NodeId dataSetFieldIdent1;
    UA_DataSetFieldConfig dataSetFieldConfig;
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    staticValueSource = UA_DataValue_new();
#endif
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
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
       UA_Server_addDataSetField(server, publishedDataSetIdent, &dataSetFieldConfig, &dataSetFieldIdent1);
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
    writerGroupConfig.name                                 = UA_STRING("Demo WriterGroup");
    writerGroupConfig.publishingInterval                   = CYCLE_TIME;
    writerGroupConfig.enabled                              = UA_FALSE;
    writerGroupConfig.encodingMimeType                     = UA_PUBSUB_ENCODING_UADP;
    writerGroupConfig.writerGroupId                        = WRITER_GROUP_ID;
#if defined PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
    writerGroupConfig.rtLevel                              = UA_PUBSUB_RT_FIXED_SIZE;
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
#endif

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
 * This routine publishes the data at a cycle time of 100us.
 */
void *publisherETF(void *arg) {
    struct timespec   nextnanosleeptime;
    UA_ServerCallback pubCallback;
    UA_Server*        server;
    UA_WriterGroup*   currentWriterGroup;
    UA_UInt64         interval_ns;
    UA_UInt64         transmission_time;

    /* Initialise value for nextnanosleeptime timespec */
    nextnanosleeptime.tv_nsec           = 0;
    threadArg *threadArgumentsPublisher = (threadArg *)arg;
    server                              = threadArgumentsPublisher->server;
    pubCallback                         = threadArgumentsPublisher->callback;
    currentWriterGroup                  = (UA_WriterGroup *)threadArgumentsPublisher->data;
    interval_ns                         = (threadArgumentsPublisher->interval_ms * MILLI_SECONDS);

    /* Get current time and compute the next nanosleeptime */
    clock_gettime(CLOCKID, &nextnanosleeptime);
    /* Variable to nano Sleep until SECONDS_SLEEP second boundary */
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
    transportSettings.content.decoded.data         = &ethernetETFtransportSettings;
    currentWriterGroup->config.transportSettings   = transportSettings;
    UA_UInt64 roundOffCycleTime                    = (CYCLE_TIME * MILLI_SECONDS) - NANO_SECONDS_SLEEP_PUB;

    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptime, NULL);
        transmission_time                              = ((UA_UInt64)nextnanosleeptime.tv_sec * SECONDS + (UA_UInt64)nextnanosleeptime.tv_nsec) + roundOffCycleTime + QBV_OFFSET;
        ethernetETFtransportSettings.transmission_time = transmission_time;
        if(*pubCounterData > 0)
            pubCallback(server, currentWriterGroup);
        nextnanosleeptime.tv_nsec                     += interval_ns;
        nanoSecondFieldConversion(&nextnanosleeptime);
    }

    UA_free(threadArgumentsPublisher);
    return (void*)NULL;
}

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
        nextnanosleeptimeSub.tv_nsec     += (CYCLE_TIME * MILLI_SECONDS);
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
/*void *userApplicationPubSub(void *arg) {
    UA_Server* server;
    struct timespec nextnanosleeptimeUserApplication;
   
    clock_gettime(CLOCKID, &nextnanosleeptimeUserApplication);
   
    nextnanosleeptimeUserApplication.tv_sec                      += SECONDS_SLEEP;
    nextnanosleeptimeUserApplication.tv_nsec                      = NANO_SECONDS_SLEEP_USER_APPLICATION;
    nanoSecondFieldConversion(&nextnanosleeptimeUserApplication);
    serverConfigStruct *serverConfig = (serverConfigStruct*)arg;
    server = serverConfig->ServerRun;
    while (running) {
        clock_nanosleep(CLOCKID, TIMER_ABSTIME, &nextnanosleeptimeUserApplication, NULL);
#if defined(SUBSCRIBER)
       // const UA_NodeId nodeid = UA_NODEID_STRING(1, "SubscriberCounter");
      //  UA_Variant subCounter;
       // UA_Variant_init(&subCounter);
        //UA_Server_readValue(server, nodeid, &subCounter);
        clock_gettime(CLOCKID, &dataReceiveTime);
        subCounterData = 410;
    //    UA_Variant_clear(&subCounter);
        for (UA_Int32 iterator = 0; iterator <  REPEATED_NODECOUNTS; iterator++)
        {
            UA_Variant_init(&subCounter);
            UA_Server_readValue(server, UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+50000), &subCounter);
            *repeatedCounterData[iterator] = *(UA_UInt64 *)subCounter.data;
            UA_Variant_clear(&subCounter);
        }
#endif
#if defined(PUBLISHER)
        clock_gettime(CLOCKID, &dataModificationTime);
        *pubCounterData = subCounterData;
#ifndef PUBSUB_CONFIG_FASTPATH_FIXED_OFFSETS
        UA_Variant pubCounter;
        UA_Variant_init(&pubCounter);
        UA_Variant_setScalar(&pubCounter, pubCounterData, &UA_TYPES[UA_TYPES_UINT64]);
        UA_NodeId currentNodeId         = UA_NODEID_STRING(1, "PublisherCounter");
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
#if defined(UPDATE_MEASUREMENTS)
        if (subCounterData > 0)
             updateMeasurementsSubscriber(dataReceiveTime, subCounterData);
        if (*pubCounterData > 0)
             updateMeasurementsPublisher(dataModificationTime, *pubCounterData);
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
}*/
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

    
        UA_Server_deleteNode(server, rtpostionNode, UA_TRUE);
        UA_Server_deleteNode(server, CycletimeTSNNode, UA_TRUE);
        UA_Server_deleteNode(server, QbvTSNNode, UA_TRUE);
        UA_Server_deleteNode(server, stopcurrentlyNode, UA_TRUE);
        UA_Server_deleteNode(server, resetNode, UA_TRUE);
        UA_Server_deleteNode(server, backtozeroNode, UA_TRUE);
        UA_Server_deleteNode(server, moveNode, UA_TRUE);
        UA_Server_deleteNode(server, statusNode, UA_TRUE);
         UA_Server_deleteNode(server,settingTSNNode, UA_TRUE);
        UA_Server_deleteNode(server, numberNode, UA_TRUE);
         UA_Server_deleteNode(server, RefrigerationId, UA_TRUE);
        UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3910), UA_TRUE);
        UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3911), UA_TRUE);
         UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3912), UA_TRUE);
          UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3913), UA_TRUE);
           UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3914), UA_TRUE);
            UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3915), UA_TRUE);
             UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3916), UA_TRUE);
              UA_Server_deleteNode(server, UA_NODEID_NUMERIC(1, 3917), UA_TRUE);
      
         UA_NodeId_clear(&RefrigerationId);
        UA_NodeId_clear(&rtpostionNode);
        UA_NodeId_clear(&CycletimeTSNNode);
        UA_NodeId_clear(&QbvTSNNode);
        UA_NodeId_clear(&stopcurrentlyNode);
        UA_NodeId_clear(&resetNode);
        UA_NodeId_clear(&backtozeroNode);
        UA_NodeId_clear(&moveNode);
        UA_NodeId_clear(&statusNode);
         UA_NodeId_clear(&settingTSNNode);
        UA_NodeId_clear(&numberNode);
    


  /*  for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)//YXHGL
    {
        UA_Server_deleteNode(server, subRepeatedCountNodeID, UA_TRUE);
        UA_NodeId_clear(&subRepeatedCountNodeID);
    }*/
}

static pthread_t threadCreation(UA_Int16 threadPriority, size_t coreAffinity, void *(*thread) (void *), char *applicationName, \
                                void *serverConfig){
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
 * The addServerNodes function is used to create the publisher and subscriber
 * nodes.
 */
/*static void addServerNodes(UA_Server *server) {
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
//YXHGL
    for (UA_Int32 iterator = 0; iterator < REPEATED_NODECOUNTS; iterator++)
    {
        UA_VariableAttributes repeatedNodeSub = UA_VariableAttributes_default;
        UA_DateTime repeatedSubscribeValue;
        UA_Variant_setScalar(&repeatedNodeSub.value, &repeatedSubscribeValue, &UA_TYPES[UA_TYPES_UINT64]);
        repeatedNodeSub.accessLevel           = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        repeatedNodeSub.displayName           = UA_LOCALIZEDTEXT("en-US", "Subscriber RepeatedCounter");
        newNodeId                             = UA_NODEID_NUMERIC(1, (UA_UInt32)iterator+50000);
        UA_Server_addVariableNode(server, newNodeId, objectId,
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                  UA_QUALIFIEDNAME(1, "Subscriber RepeatedCounter"),
                                  UA_NODEID_NULL, repeatedNodeSub, NULL, &subRepeatedCountNodeID);
    }

}*/

static void
usage(char *progname) {
    printf("usage: %s <ethernet_interface> \n", progname);
    printf("Provide the Interface parameter to run the application. Exiting \n");
}
/*信息模型*/
static void
DefineeDevice(UA_Server *server) {
    
    UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
    oAttr.displayName = UA_LOCALIZEDTEXT("en-US", "电机设备");
    UA_Server_addObjectNode(server, RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, "Refrigeration equipment"), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
                            oAttr, NULL, &RefrigerationId);


    UA_VariableAttributes nbAttr = UA_VariableAttributes_default;
    UA_Int32 value = 01;
    UA_Variant_setScalar(&nbAttr.value, &value, &UA_TYPES[UA_TYPES_INT32]);
    nbAttr.displayName = UA_LOCALIZEDTEXT("en-US", "编号");
    nbAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, numberNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "number"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), nbAttr, NULL, NULL);

    UA_VariableAttributes rttAttr = UA_VariableAttributes_default;
    UA_Double rttvalue = 00.00;
    UA_Variant_setScalar(&rttAttr.value, &rttvalue, &UA_TYPES[UA_TYPES_DOUBLE]);
    rttAttr.displayName = UA_LOCALIZEDTEXT("en-US", "位置");
    rttAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, rtpostionNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Real time position"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), rttAttr, NULL, NULL);

    UA_VariableAttributes stAttr = UA_VariableAttributes_default;
    UA_String TSNvalue = UA_STRING("test");
    UA_Variant_setScalar(&stAttr.value, &TSNvalue, &UA_TYPES[UA_TYPES_STRING]);
    stAttr.displayName = UA_LOCALIZEDTEXT("en-US", "TSN配置");
    stAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, settingTSNNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Setting TSN"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), stAttr, NULL, NULL);

    UA_VariableAttributes stAttr1 = UA_VariableAttributes_default;
    UA_Double TSNvalue1 = 0.01;
    UA_Variant_setScalar(&stAttr1.value, &TSNvalue1, &UA_TYPES[UA_TYPES_DOUBLE]);
    stAttr1.displayName = UA_LOCALIZEDTEXT("en-US", "循环时间");
    stAttr1.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, CycletimeTSNNode, settingTSNNode,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Cycle Time"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), stAttr1, NULL, NULL);

    UA_VariableAttributes stAttr2 = UA_VariableAttributes_default;
    UA_Int16 TSNvalue2 = 100;
    UA_Variant_setScalar(&stAttr2.value, &TSNvalue2, &UA_TYPES[UA_TYPES_INT16]);
    stAttr2.displayName = UA_LOCALIZEDTEXT("en-US", "Qbv Offset");
    stAttr2.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    UA_Server_addVariableNode(server, QbvTSNNode, settingTSNNode,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Qbv Offset"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), stAttr2, NULL, NULL);

    UA_VariableAttributes statusAttr = UA_VariableAttributes_default;
    UA_Boolean statusvalue = 0;
    UA_Variant_setScalar(&statusAttr.value, &statusvalue, &UA_TYPES[UA_TYPES_BOOLEAN]);
    statusAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    statusAttr.displayName = UA_LOCALIZEDTEXT("en-US", "使能");
    UA_Server_addVariableNode(server, statusNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Status"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), statusAttr, NULL, NULL);

    UA_VariableAttributes stopcurrentlyAttr = UA_VariableAttributes_default;
    UA_Boolean stopcurrentlyvalue = 0;
    UA_Variant_setScalar(&stopcurrentlyAttr.value, &stopcurrentlyvalue, &UA_TYPES[UA_TYPES_BOOLEAN]);
    stopcurrentlyAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    stopcurrentlyAttr.displayName = UA_LOCALIZEDTEXT("en-US", "急停");
    UA_Server_addVariableNode(server, stopcurrentlyNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Stopcurrently"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), stopcurrentlyAttr, NULL, NULL);

    UA_VariableAttributes resetAttr = UA_VariableAttributes_default;
    UA_Boolean resetvalue = 0;
    UA_Variant_setScalar(&resetAttr.value, &resetvalue, &UA_TYPES[UA_TYPES_BOOLEAN]);
    resetAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    resetAttr.displayName = UA_LOCALIZEDTEXT("en-US", "复位");
    UA_Server_addVariableNode(server, resetNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Reset"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), resetAttr, NULL, NULL);

    UA_VariableAttributes backtozeroAttr = UA_VariableAttributes_default;
    UA_Boolean backtozerovalue = 0;
    UA_Variant_setScalar(&backtozeroAttr.value, &backtozerovalue, &UA_TYPES[UA_TYPES_BOOLEAN]);
    backtozeroAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    backtozeroAttr.displayName = UA_LOCALIZEDTEXT("en-US", "回零");
    UA_Server_addVariableNode(server, backtozeroNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Backtozero"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), backtozeroAttr, NULL, NULL);

    UA_VariableAttributes moveAttr = UA_VariableAttributes_default;
    UA_Boolean movevalue = 0;
    UA_Variant_setScalar(&moveAttr.value, &movevalue, &UA_TYPES[UA_TYPES_BOOLEAN]);
    moveAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    moveAttr.displayName = UA_LOCALIZEDTEXT("en-US", "运动");
    UA_Server_addVariableNode(server, moveNode, RefrigerationId,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_QUALIFIEDNAME(1, "Move"),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), moveAttr, NULL, NULL);

}

//方法回调和方法成对使用
//设置循环时间
static UA_StatusCode
settingCycletimeTSNCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

  
    UA_Double tmp = *(UA_Double*)input->data;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &tmp, &UA_TYPES[UA_TYPES_DOUBLE]);
    UA_Server_writeValue(server, CycletimeTSNNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addsettingCycletimeTSNMethod(UA_Server *server) {

    UA_Argument inputArgument;
    UA_Argument_init(&inputArgument);
    inputArgument.description = UA_LOCALIZEDTEXT("en-US", "A Double");
    inputArgument.name = UA_STRING("MyInput");
    inputArgument.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    inputArgument.valueRank = UA_VALUERANK_SCALAR;

    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","设定循环时间入口");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3910),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &settingCycletimeTSNCallback,
                            1, &inputArgument, 0, NULL, NULL, NULL);
}

//设置QBV Offeset
static UA_StatusCode
settingQBVTSNCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

  
    UA_Int16 tmp = *(UA_Int16*)input->data;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &tmp, &UA_TYPES[UA_TYPES_INT16]);
    UA_Server_writeValue(server, QbvTSNNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addsettingQBVTSNMethod(UA_Server *server) {
    UA_Argument inputArgument;
    UA_Argument_init(&inputArgument);
    inputArgument.description = UA_LOCALIZEDTEXT("en-US", "A Int16");
    inputArgument.name = UA_STRING("MyInput");
    inputArgument.dataType = UA_TYPES[UA_TYPES_INT16].typeId;
    inputArgument.valueRank = UA_VALUERANK_SCALAR;

    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","设定QBV Offset入口");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3911),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &settingQBVTSNCallback,
                            1, &inputArgument, 0, NULL, NULL, NULL);
}


//使能标致
static UA_StatusCode
shutdownCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

   static UA_Boolean shutdownstatus = false;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &shutdownstatus, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_Server_writeValue(server, statusNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addsettingshutdownMethod(UA_Server *server) {
    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","关闭");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3912),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &shutdownCallback,
                            0, NULL, 0, NULL, NULL, NULL);
}

static UA_StatusCode
setupCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

   static UA_Boolean shutdownstatus = true;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &shutdownstatus, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_Server_writeValue(server, statusNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addsettingsetupMethod(UA_Server *server) {
    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","启动");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3913),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &setupCallback,
                            0, NULL, 0, NULL, NULL, NULL);
}

//急停方法
static UA_StatusCode
StopcurrentlyCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

   static UA_Boolean shutdownstatus = true;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &shutdownstatus, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_Server_writeValue(server, stopcurrentlyNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addstopcurrentlyMethod(UA_Server *server) {
    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","急停");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3914),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &StopcurrentlyCallback,
                            0, NULL, 0, NULL, NULL, NULL);
}



//复位方法
static UA_StatusCode
ResetCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

   static UA_Boolean shutdownstatus = true;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &shutdownstatus, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_Server_writeValue(server, resetNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addresetMethod(UA_Server *server) {
    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","复位");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3915),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &ResetCallback,
                            0, NULL, 0, NULL, NULL, NULL);
}


//回零方法
static UA_StatusCode
BacktozeroCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

   static UA_Boolean shutdownstatus = true;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &shutdownstatus, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_Server_writeValue(server, backtozeroNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addbacktozeroMethod(UA_Server *server) {
    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","回零");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3916),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &BacktozeroCallback,
                            0, NULL, 0, NULL, NULL, NULL);
}

//运动方法
static UA_StatusCode
MoveCallback(UA_Server *server,
                         const UA_NodeId *sessionId, void *sessionHandle,
                         const UA_NodeId *methodId, void *methodContext,
                         const UA_NodeId *objectId, void *objectContext,
                         size_t inputSize, const UA_Variant *input,
                         size_t outputSize, UA_Variant *output) {

    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "Creating event");

   static UA_Boolean shutdownstatus = true;
    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &shutdownstatus, &UA_TYPES[UA_TYPES_BOOLEAN]);
    UA_Server_writeValue(server, moveNode, myVar);

    return UA_STATUSCODE_GOOD;
}


static void
addmoveMethod(UA_Server *server) {
    UA_MethodAttributes generateAttr = UA_MethodAttributes_default;
    generateAttr.description = UA_LOCALIZEDTEXT("en-US","Generate an event.");
    generateAttr.displayName = UA_LOCALIZEDTEXT("en-US","运动");
    generateAttr.executable = true;
    generateAttr.userExecutable = true;
    UA_Server_addMethodNode(server, UA_NODEID_NUMERIC(1, 3917),
                            RefrigerationId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_HASORDEREDCOMPONENT),
                            UA_QUALIFIEDNAME(1, "Generate Event"),
                            generateAttr, &MoveCallback,
                            0, NULL, 0, NULL, NULL, NULL);
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
numberNode = UA_NODEID_NUMERIC(1, 39997);
rtpostionNode = UA_NODEID_NUMERIC(1,  39998);
settingTSNNode =  UA_NODEID_NUMERIC(1,  39999);
CycletimeTSNNode = UA_NODEID_NUMERIC(1,  40000);
QbvTSNNode =  UA_NODEID_NUMERIC(1,  40001);
statusNode =  UA_NODEID_NUMERIC(1,  40002);
stopcurrentlyNode = UA_NODEID_NUMERIC(1, 40003);
resetNode = UA_NODEID_NUMERIC(1,  40004);
backtozeroNode =  UA_NODEID_NUMERIC(1,  40005);
moveNode =  UA_NODEID_NUMERIC(1,  40006);
    UA_Int32         returnValue         = 0;
    UA_StatusCode    retval              = UA_STATUSCODE_GOOD;
    UA_Server       *server              = UA_Server_new();
    UA_ServerConfig *config              = UA_Server_getConfig(server);
//    pthread_t        userThreadID;
    UA_ServerConfig_setMinimal(config, PORT_NUMBER, NULL);

#if defined(PUBLISHER)
    UA_NetworkAddressUrlDataType networkAddressUrlPub;
#endif
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

#if defined(PUBLISHER)
#if defined(UPDATE_MEASUREMENTS)
    fpPublisher                   = fopen(filePublishedData, "w");
#endif
#endif
#if defined(SUBSCRIBER)
#if defined(UPDATE_MEASUREMENTS)
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

    /* Server is the new OPCUA model which has both publisher and subscriber configuration */
    /* add axis node and OPCUA pubsub client server counter nodes */
 //   addServerNodes(server);
/*添加信息模型*/
    DefineeDevice(server);
    addsettingshutdownMethod(server);
    addsettingsetupMethod(server);
    addsettingCycletimeTSNMethod(server);
    addsettingQBVTSNMethod(server);
    addstopcurrentlyMethod(server);
    addresetMethod(server);
    addbacktozeroMethod(server);
    addmoveMethod(server);
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
    serverConfig                = (serverConfigStruct*)UA_malloc(sizeof(serverConfigStruct));
    serverConfig->ServerRun     = server;
#if defined(PUBLISHER) || defined(SUBSCRIBER)
   // char threadNameUserAppl[22] = "UserApplicationPubSub";
    //userThreadID                = threadCreation(USERAPPLICATION_SCHED_PRIORITY, CORE_THREE, userApplicationPubSub, threadNameUserAppl, serverConfig);
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
   /* returnValue = pthread_join(userThreadID, NULL);
    if (returnValue != 0) {
        UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND,"\nPthread Join Failed for User thread:%d\n", returnValue);
    }*/
#endif

#if defined(PUBLISHER)
#if defined(UPDATE_MEASUREMENTS)
    /* Write the published data in the publisher_T1.csv file */
    size_t pubLoopVariable = 0;
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
    size_t subLoopVariable = 0;
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
