#include "gtest/gtest.h"
#include "../src/source/Include_i.h"
#include <memory>
#include <thread>

#define TEST_DEFAULT_REGION                                     ((PCHAR) "us-west-2")
#define TEST_STREAMING_TOKEN_DURATION                           (40 * HUNDREDS_OF_NANOS_IN_A_SECOND)
#define TEST_JITTER_BUFFER_CLOCK_RATE                           (1000)
#define TEST_SIGNALING_MASTER_CLIENT_ID                         (PCHAR) "Test_Master_ClientId"
#define TEST_SIGNALING_VIEWER_CLIENT_ID                         (PCHAR) "Test_Viewer_ClientId"
#define TEST_SIGNALING_CHANNEL_NAME                             (PCHAR) "ScaryTestChannel_"
#define SIGNAING_TEST_CORRELATION_ID                            (PCHAR) "Test_correlation_id"
#define TEST_SIGNALING_MESSAGE_TTL                              (120 * HUNDREDS_OF_NANOS_IN_A_SECOND)

//
// Set the allocators to the instrumented equivalents
//
extern memAlloc globalMemAlloc;
extern memAlignAlloc globalMemAlignAlloc;
extern memCalloc globalMemCalloc;
extern memFree globalMemFree;

namespace com { namespace amazonaws { namespace kinesis { namespace video { namespace webrtcclient {
//
// Default allocator functions
//
extern UINT64 gTotalWebRtcClientMemoryUsage;
extern MUTEX gTotalWebRtcClientMemoryMutex;

INLINE PVOID instrumentedWebRtcClientMemAlloc(SIZE_T size)
{
    DLOGS("Test malloc %llu bytes", (UINT64)size);
    MUTEX_LOCK(gTotalWebRtcClientMemoryMutex);
    gTotalWebRtcClientMemoryUsage += size;
    MUTEX_UNLOCK(gTotalWebRtcClientMemoryMutex);
    PBYTE pAlloc = (PBYTE) malloc(size + SIZEOF(SIZE_T));
    *(PSIZE_T)pAlloc = size;

    return pAlloc + SIZEOF(SIZE_T);
}

INLINE PVOID instrumentedWebRtcClientMemAlignAlloc(SIZE_T size, SIZE_T alignment)
{
    DLOGS("Test align malloc %llu bytes", (UINT64)size);
    // Just do malloc
    UNUSED_PARAM(alignment);
    return instrumentedWebRtcClientMemAlloc(size);
}

INLINE PVOID instrumentedWebRtcClientMemCalloc(SIZE_T num, SIZE_T size)
{
    SIZE_T overallSize = num * size;
    DLOGS("Test calloc %llu bytes", (UINT64)overallSize);
    MUTEX_LOCK(gTotalWebRtcClientMemoryMutex);
    gTotalWebRtcClientMemoryUsage += overallSize;
    MUTEX_UNLOCK(gTotalWebRtcClientMemoryMutex);

    PBYTE pAlloc = (PBYTE) calloc(1, overallSize + SIZEOF(SIZE_T));
    *(PSIZE_T)pAlloc = overallSize;

    return pAlloc + SIZEOF(SIZE_T);
}

INLINE VOID instrumentedWebRtcClientMemFree(PVOID ptr)
{
    PBYTE pAlloc = (PBYTE) ptr - SIZEOF(SIZE_T);
    SIZE_T size = *(PSIZE_T) pAlloc;
    DLOGS("Test free %llu bytes", (UINT64)size);

    MUTEX_LOCK(gTotalWebRtcClientMemoryMutex);
    gTotalWebRtcClientMemoryUsage -= size;
    MUTEX_UNLOCK(gTotalWebRtcClientMemoryMutex);

    free(pAlloc);
}

class WebRtcClientTestBase : public ::testing::Test {
public:
    PUINT32 mExpectedFrameSizeArr;
    PBYTE *mPExpectedFrameArr;
    UINT32 mExpectedFrameCount;
    PUINT32 mExpectedDroppedFrameTimestampArr;
    UINT32 mExpectedDroppedFrameCount;
    PRtpPacket* mPRtpPackets;
    UINT32 mRtpPacketCount;
    SIGNALING_CLIENT_HANDLE mSignalingClientHandle;

    WebRtcClientTestBase();

    PCHAR getAccessKey()
    {
        return mAccessKey;
    }

    PCHAR getSecretKey()
    {
        return mSecretKey;
    }

    PCHAR getSessionToken()
    {
        return mSessionToken;
    }

    STATUS initializeSignalingClient() {
        ChannelInfo channelInfo;
        SignalingClientCallbacks signalingClientCallbacks;
        SignalingClientInfo clientInfo;
        Tag tags[3];
        STATUS retStatus;

        tags[0].version = TAG_CURRENT_VERSION;
        tags[0].name = (PCHAR) "Tag Name 0";
        tags[0].value = (PCHAR) "Tag Value 0";
        tags[1].version = TAG_CURRENT_VERSION;
        tags[1].name = (PCHAR) "Tag Name 1";
        tags[1].value = (PCHAR) "Tag Value 1";
        tags[2].version = TAG_CURRENT_VERSION;
        tags[2].name = (PCHAR) "Tag Name 2";
        tags[2].value = (PCHAR) "Tag Value 2";

        signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
        signalingClientCallbacks.customData = (UINT64) this;
        signalingClientCallbacks.messageReceivedFn = NULL;
        signalingClientCallbacks.errorReportFn = NULL;
        signalingClientCallbacks.stateChangeFn = NULL;

        clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
        clientInfo.loggingLevel = LOG_LEVEL_VERBOSE;
        STRCPY(clientInfo.clientId, TEST_SIGNALING_MASTER_CLIENT_ID);

        MEMSET(&channelInfo, 0x00, SIZEOF(ChannelInfo));
        channelInfo.version = CHANNEL_INFO_CURRENT_VERSION;
        channelInfo.pChannelName = mChannelName;
        channelInfo.pKmsKeyId = NULL;
        channelInfo.tagCount = 3;
        channelInfo.pTags = tags;
        channelInfo.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
        channelInfo.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
        channelInfo.cachingEndpoint = FALSE;
        channelInfo.retry = TRUE;
        channelInfo.reconnect = TRUE;
        channelInfo.pCertPath = mCaCertPath;
        channelInfo.messageTtl = TEST_SIGNALING_MESSAGE_TTL;

        retStatus = createSignalingClientSync(&clientInfo, &channelInfo, &signalingClientCallbacks,
                (PAwsCredentialProvider) mTestCredentialProvider,
                &mSignalingClientHandle);

        if (mAccessKeyIdSet) {
            EXPECT_EQ(STATUS_SUCCESS, retStatus);
        } else {
            EXPECT_NE(STATUS_SUCCESS, retStatus);
        }

        return retStatus;
    }

    STATUS deinitializeSignalingClient() {

        // Delete the created channel
        if (mAccessKeyIdSet) {
            deleteChannelLws(FROM_SIGNALING_CLIENT_HANDLE(mSignalingClientHandle), 0);
        }

        EXPECT_EQ(STATUS_SUCCESS, freeSignalingClient(&mSignalingClientHandle));

        return STATUS_SUCCESS;
    }


    static STATUS testFrameReadyFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
    {
        WebRtcClientTestBase* base = (WebRtcClientTestBase*) customData;
        UINT32 filledSize;
        EXPECT_GT(base->mExpectedFrameCount, base->mReadyFrameIndex);
        EXPECT_EQ(base->mExpectedFrameSizeArr[base->mReadyFrameIndex], frameSize);
        if (base->mFrame != NULL) {
            MEMFREE(base->mFrame);
            base->mFrame = NULL;
        }
        base->mFrame = (PBYTE) MEMALLOC(frameSize);
        EXPECT_EQ(STATUS_SUCCESS, jitterBufferFillFrameData(base->mJitterBuffer, base->mFrame, frameSize, &filledSize, startIndex, endIndex));
        EXPECT_EQ(frameSize, filledSize);
        EXPECT_EQ(0, MEMCMP(base->mPExpectedFrameArr[base->mReadyFrameIndex], base->mFrame, frameSize));
        base->mReadyFrameIndex++;
        return STATUS_SUCCESS;
    }

    static STATUS testFrameDroppedFunc(UINT64 customData, UINT32 timestamp)
    {
        WebRtcClientTestBase* base = (WebRtcClientTestBase*) customData;
        EXPECT_GT(base->mExpectedDroppedFrameCount, base->mDroppedFrameIndex);
        EXPECT_EQ(base->mExpectedDroppedFrameTimestampArr[base->mDroppedFrameIndex], timestamp);
        base->mDroppedFrameIndex++;
        return STATUS_SUCCESS;
    }

    static STATUS testDepayRtpFunc(PBYTE payload, UINT32 payloadLength, PBYTE outBuffer, PUINT32 pBufferSize, PBOOL pIsStart)
    {
        ENTERS();
        STATUS retStatus = STATUS_SUCCESS;
        UINT32 bufferSize = 0;
        BOOL sizeCalculationOnly = (outBuffer == NULL);

        UNUSED_PARAM(pIsStart);
        CHK(payload != NULL && pBufferSize != NULL, STATUS_NULL_ARG);
        CHK(payloadLength > 0, retStatus);

        bufferSize = payloadLength;

        // Only return size if given buffer is NULL
        CHK(!sizeCalculationOnly, retStatus);
        CHK(payloadLength <= *pBufferSize, STATUS_BUFFER_TOO_SMALL);

        MEMCPY(outBuffer, payload, payloadLength);

    CleanUp:
        if (STATUS_FAILED(retStatus) && sizeCalculationOnly) {
            bufferSize = 0;
        }

        if (pBufferSize != NULL) {
            *pBufferSize = bufferSize;
        }

        if (pIsStart != NULL) {
            *pIsStart = (payload[payloadLength] != 0);
        }

        LEAVES();
        return retStatus;
    }

protected:

    virtual void SetUp();
    virtual void TearDown();
    PCHAR GetTestName();
    VOID initializeJitterBuffer(UINT32, UINT32, UINT32);
    VOID clearJitterBufferForTest();
    VOID setPayloadToFree();

    // Stored function pointers to reset on exit
    memAlloc mStoredMemAlloc;
    memAlignAlloc mStoredMemAlignAlloc;
    memCalloc mStoredMemCalloc;
    memFree mStoredMemFree;

    PAwsCredentialProvider mTestCredentialProvider;

    PCHAR mAccessKey;
    PCHAR mSecretKey;
    PCHAR mSessionToken;
    PCHAR mRegion;
    PCHAR mCaCertPath;
    UINT64 mStreamingRotationPeriod;

    CHAR mDefaultRegion[MAX_REGION_NAME_LEN + 1];
    BOOL mAccessKeyIdSet;
    CHAR mChannelName[MAX_CHANNEL_NAME_LEN + 1];

    PJitterBuffer mJitterBuffer;
    PBYTE mFrame;
    UINT32 mReadyFrameIndex;
    UINT32 mDroppedFrameIndex;
};

}  // namespace webrtcclient
}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;
