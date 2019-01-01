#pragma once

#include <ntstrsafe.h>

typedef struct _BTHPS3_CONNECTION * PBTHPS3_CONNECTION;

#define BTHPS3_NUM_CONTINUOUS_READERS 2

typedef VOID
(*PFN_BTHPS3_CONNECTION_OBJECT_CONTREADER_READ_COMPLETE) (
    _In_ PBTHPS3_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ PBTHPS3_CONNECTION Connection,
    _In_ PVOID Buffer,
    _In_ size_t BufferSize
    );

typedef VOID
(*PFN_BTHPS3_CONNECTION_OBJECT_CONTREADER_FAILED) (
    _In_ PBTHPS3_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ PBTHPS3_CONNECTION Connection
    );


typedef struct _BTHPS3_REPEAT_READER
{
    //
    // BRB used for transfer
    //
    struct _BRB_L2CA_ACL_TRANSFER TransferBrb;

    //
    // WDF Request used for pending I/O
    //
    WDFREQUEST RequestPendingRead;

    //
    // Data buffer for pending read
    //
    WDFMEMORY MemoryPendingRead;

    //
    // Dpc for resubmitting pending read
    //
    KDPC ResubmitDpc;

    //
    // Whether the continuous reader is transitioning to stopped state
    //

    LONG Stopping;

    //
    // Event used to wait for read completion while stopping the continuos reader
    //
    KEVENT StopEvent;

    //
    // Back pointer to connection
    //
    PBTHPS3_CONNECTION Connection;

} BTHPS3_REPEAT_READER, *PBTHPS3_REPEAT_READER;

//
// Connection state
//

typedef enum _BTHPS3_CONNECTION_STATE {
    ConnectionStateUnitialized = 0,
    ConnectionStateInitialized,
    ConnectionStateConnecting,
    ConnectionStateConnected,
    ConnectionStateConnectFailed,
    ConnectionStateDisconnecting,
    ConnectionStateDisconnected

} BTHPS3_CONNECTION_STATE, *PBTHPS3_CONNECTION_STATE;

typedef struct _BTHPS3_CONTINUOUS_READER {

    BTHPS3_REPEAT_READER
        RepeatReaders[BTHPS3_NUM_CONTINUOUS_READERS];

    PFN_BTHPS3_CONNECTION_OBJECT_CONTREADER_READ_COMPLETE
        BthEchoConnectionObjectContReaderReadCompleteCallback;

    PFN_BTHPS3_CONNECTION_OBJECT_CONTREADER_FAILED
        BthEchoConnectionObjectContReaderFailedCallback;

    DWORD                                   InitializedReadersCount;

} BTHPS3_CONTINUOUS_READER, *PBTHPS3_CONTINUOUS_READER;

//
// Connection data structure for L2Ca connection
//

typedef struct _BTHPS3_CONNECTION {

    //
    // List entry for connection list maintained at device level
    //
    LIST_ENTRY                              ConnectionListEntry;

    PBTHPS3_DEVICE_CONTEXT_HEADER    DevCtxHdr;

    BTHPS3_CONNECTION_STATE          ConnectionState;

    //
    // Connection lock, used to synchronize access to _BTHECHO_CONNECTION data structure
    //
    WDFSPINLOCK                             ConnectionLock;

    USHORT                                  OutMTU;
    USHORT                                  InMTU;

    L2CAP_CHANNEL_HANDLE                    ChannelHandle;
    BTH_ADDR                                RemoteAddress;

    //
    // Preallocated Brb, Request used for connect/disconnect
    //
    struct _BRB                             ConnectDisconnectBrb;
    WDFREQUEST                              ConnectDisconnectRequest;

    //
    // Event used to wait for disconnection
    // It is non-signaled when connection is in ConnectionStateDisconnecting
    // transitionary state and signaled otherwise
    //
    KEVENT                                  DisconnectEvent;

    //
    // Continuous readers (used only by server)
    // PLEASE NOTE that KMDF USB Pipe Target uses a single continuous reader
    //
    BTHPS3_CONTINUOUS_READER               ContinuousReader;
} BTHPS3_CONNECTION, *PBTHPS3_CONNECTION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BTHPS3_CONNECTION, GetConnectionObjectContext)


_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
BthPS3ConnectionObjectInit(
    _In_ WDFOBJECT ConnectionObject,
    _In_ PBTHPS3_DEVICE_CONTEXT_HEADER DevCtxHdr
);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
BthPS3ConnectionObjectCreate(
    _In_ PBTHPS3_DEVICE_CONTEXT_HEADER DevCtxHdr,
    _In_ WDFOBJECT ParentObject,
    _Out_ WDFOBJECT*  ConnectionObject
);
