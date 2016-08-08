#ifndef ZP_CONST_H
#define ZP_CONST_H

#include <string>

const int kMaxWorkerThread = 4;
const int kMaxMetaWorkerThread = 16;

const std::string kZPVersion = "0.0.1";
const std::string kZPPidFile = "zp.pid";

////// Server State /////
enum Role {
  kNodeSingle = 0,
  kNodeMaster = 1,
  kNodeSlave = 2,
};
enum ReplState {
  kNoConnect = 0,
  kShouldConnect = 1,
  kConnected = 2,
  kWaitDBSync = 3,
};
enum MetaState {
  kMetaConnect = 1,
  kMetaConnecting = 2,
  kMetaConnected = 3,
};

// Data port shift
const int kPortShiftDataCmd = 100;
const int kPortShiftSync = 200;
const int kPortShiftRsync = 300;

// Meta port shift
const int kMetaPortShiftCmd = 100;
const int kMetaPortShiftFY = 200;


const int kTrySyncInterval = 2;
const int kPingInterval = 3;
const int kMetaCmdCronInterval = 1;
const int kDispatchCronInterval = 5000;
const int kMetaDispathCronInterval = 3000;
const int kWorkerCronInterval = 5000;
const int kMetaWorkerCronInterval = 1000;
const int kBinlogReceiverCronInterval = 6000;
//const int kBinlogReceiverCronInterval = 1000;



////// Binlog related //////
// the block size that we read and write from write2file
// the default size is 64KB
const size_t kBlockSize = 64 * 1024;

// Header is Type(1 byte), length (2 bytes)
const size_t kHeaderSize = 1 + 3;

const std::string kBinlogPrefix = "binlog";
//const size_t kBinlogPrefixLen = 6;

const std::string kManifest = "manifest";

//#define SLAVE_ITEM_STAGE_ONE 1
//#define SLAVE_ITEM_STAGE_TWO 2

/*
 * The size of Binlogfile
 */
//uint64_t kBinlogSize = 128; 
//const uint64_t kBinlogSize = 1024 * 1024 * 100;


/*
 * define reply between master and slave
 *
 */
const std::string kInnerReplOk = "ok";
const std::string kInnerReplWait = "wait";

const unsigned int kMaxBitOpInputKey = 12800;
const int kMaxBitOpInputBit = 21;
/*
 * db sync
 */
const uint32_t kDBSyncMaxGap = 50;
const std::string kDBSyncModule = "document";

const std::string kBgsaveInfoFile = "info";


/*
 * meta related
 * key in floyd is zpmeta##id
 */
const int NODE_ALIVE_LEASE = 6;
const std::string ZP_META_KEY_PREFIX = "zpmeta##";
const int ZP_META_UPDATE_RETRY_TIME = 3;

#endif