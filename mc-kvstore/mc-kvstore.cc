/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <string.h>
#include <cstdlib>
#include <cctype>
#include <algorithm>

#include "mc-kvstore/mc-kvstore.hh"
#include "mc-kvstore/mc-engine.hh"
#include "ep_engine.h"
#include "tools/cJSON.h"
#include "couch_db.h"

static std::string getStringFromJSONObj(cJSON *i) {
    if (i == NULL) {
        return "";
    }
    assert(i->type == cJSON_String);
    return i->valuestring;
}


MCKVStore::MCKVStore(EventuallyPersistentEngine &theEngine) :
    KVStore(), stats(theEngine.getEpStats()), intransaction(false), mc(NULL),
    config(theEngine.getConfiguration()), engine(theEngine),
    vbBatchCount(config.getCouchVbucketBatchCount()) {

    vbBatchSize = config.getMaxTxnSize() / vbBatchCount;
    vbBatchSize = vbBatchSize == 0 ? config.getCouchDefaultBatchSize() : vbBatchSize;
    open();
}

MCKVStore::MCKVStore(const MCKVStore &from) :
    KVStore(from), stats(from.stats), intransaction(false), mc(NULL),
    config(from.config), engine(from.engine),
    vbBatchCount(from.vbBatchCount), vbBatchSize(from.vbBatchSize) {
    open();
    close_db(NULL);
}

void MCKVStore::reset() {
    // @todo what is a clean state?
    // I guess we should probably create a new message to send
    //       directly to mcd to avoid having a lot of ping-pongs
    RememberingCallback<bool> cb;
    mc->flush(cb);
    cb.waitForValue();
}

void MCKVStore::set(const Item &itm, uint16_t, Callback<mutation_result> &cb) {
    assert(intransaction);
    mc->setmq(itm, cb);
}

void MCKVStore::get(const std::string &key, uint64_t, uint16_t vb, uint16_t,
        Callback<GetValue> &cb) {
    if (dynamic_cast<RememberingCallback<GetValue> *> (&cb)) {
        mc->get(key, vb, cb);
    } else {
        RememberingCallback<GetValue> mcb;
        mc->get(key, vb, mcb);
        mcb.waitForValue();
        cb.callback(mcb.val);
    }
}

void MCKVStore::del(const Item &itm, uint64_t, uint16_t, Callback<int> &cb) {

    assert(intransaction);
    mc->delq(itm, cb);
}

bool MCKVStore::delVBucket(uint16_t vbucket, uint16_t vb_version,
        std::pair<int64_t, int64_t> row_range) {
    (void)vbucket;
    (void)vb_version;
    (void)row_range;

    bool rv = true;
    //abort();
    return rv;
}

bool MCKVStore::delVBucket(uint16_t vbucket, uint16_t) {
    RememberingCallback<bool> cb;
    mc->delVBucket(vbucket, cb);
    cb.waitForValue();
    return cb.val;
}

vbucket_map_t MCKVStore::listPersistedVbuckets() {
    RememberingCallback<std::map<std::string, std::string> > cb;
    mc->stats("vbucket", cb);

    cb.waitForValue();
    std::map<std::pair<uint16_t, uint16_t>, vbucket_state> rv;
    std::map<std::string, std::string>::const_iterator iter;
    for (iter = cb.val.begin(); iter != cb.val.end(); ++iter) {
        std::pair<uint16_t, uint16_t> vb(
                (uint16_t)atoi(iter->first.c_str() + 3), -1);
        const std::string &state_json = iter->second;
        cJSON *jsonObj = cJSON_Parse(state_json.c_str());
        std::string state = getStringFromJSONObj(cJSON_GetObjectItem(jsonObj, "state"));
        std::string checkpoint_id = getStringFromJSONObj(cJSON_GetObjectItem(jsonObj,
                                                                             "checkpoint_id"));
        cJSON_Delete(jsonObj);
        if (state.compare("") == 0 || checkpoint_id.compare("") == 0) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Warning: State JSON doc for vbucket %d is in the wrong format: %s",
                             vb.first, state_json.c_str());
            continue;
        }

        vbucket_state vb_state;
        vb_state.state = VBucket::fromString(state.c_str());
        char *ptr = NULL;
        vb_state.checkpointId = strtoull(checkpoint_id.c_str(), &ptr, 10);
        rv[vb] = vb_state;
    }

    return rv;
}

void MCKVStore::vbStateChanged(uint16_t vbucket, vbucket_state_t newState) {
    RememberingCallback<bool> cb;
    mc->setVBucket(vbucket, newState, cb);
    cb.waitForValue();
}

bool MCKVStore::snapshotVBuckets(const vbucket_map_t &m) {
    if (m.size() == 0) {
        return true;
    }
    hrtime_t start = gethrtime();
    RememberingCallback<bool> cb;
    mc->snapshotVBuckets(m, cb);
    cb.waitForValue();
    stats.snapshotVbucketHisto.add((gethrtime() - start) / 1000);
    return cb.val;
}

bool MCKVStore::snapshotStats(const std::map<std::string, std::string> &m) {
    (void)m;
    // abort();
    return true;
}

void MCKVStore::dump(shared_ptr<Callback<GetValue> > cb) {
    shared_ptr<RememberingCallback<bool> > wait(new RememberingCallback<bool>());
    shared_ptr<TapCallback> callback(new TapCallback(cb, wait));
    mc->tap(callback);
    if (!isKeyDumpSupported()) {
        wait->waitForValue();
    }
}

void MCKVStore::dump(uint16_t vb, shared_ptr<Callback<GetValue> > cb) {
    shared_ptr<RememberingCallback<bool> > wait(new RememberingCallback<bool>());
    shared_ptr<TapCallback> callback(new TapCallback(cb, wait));
    std::vector<uint16_t> vbids;
    vbids.push_back(vb);
    mc->tap(vbids, true, callback);
    wait->waitForValue();
}

void MCKVStore::dumpKeys(const std::vector<uint16_t> &vbids,  shared_ptr<Callback<GetValue> > cb) {
    shared_ptr<RememberingCallback<bool> > wait(new RememberingCallback<bool>());
    shared_ptr<TapCallback> callback(new TapCallback(cb, wait));
    (void)vbids;
    mc->tapKeys(callback);
    wait->waitForValue();
}

StorageProperties MCKVStore::getStorageProperties() {
    size_t concurrency(10);
    StorageProperties rv(concurrency, concurrency - 1, 1, true, true);
    return rv;
}

void MCKVStore::open() {
    // Wake Up!
    intransaction = false;
    delete mc;
    mc = new MemcachedEngine(&engine, config);
    RememberingCallback<bool> cb;
    mc->setVBucketBatchCount(vbBatchCount, &cb);
    cb.waitForValue();
}

void MCKVStore::close() {
    intransaction = false;
    delete mc;
    mc = NULL;
}


bool MCKVStore::commit(void) {
    // Trying to commit without an begin is a semantically bogus
    // way to do stuff
    assert(intransaction);

    RememberingCallback<bool> wait;
    mc->noop(wait);
    wait.waitForValue();
    intransaction = wait.val ? false : true;
    // This is somewhat bogus, because we don't support "real"
    // transactions.. Some of the objects in the transaction
    // may have failed so the caller needs to check the
    // callback status for all of the items it added..
    return !intransaction;
}

void MCKVStore::addStats(const std::string &prefix,
                         ADD_STAT add_stat,
                         const void *c)
{
    KVStore::addStats(prefix, add_stat, c);
    addStat(prefix, "vbucket_batch_count", vbBatchCount, add_stat, c);
    addStat(prefix, "vbucket_batch_size", vbBatchSize, add_stat, c);
    mc->addStats(prefix, add_stat, c);
}

template <typename T>
void MCKVStore::addStat(const std::string &prefix, const char *nm, T val,
                        ADD_STAT add_stat, const void *c) {
    std::stringstream name;
    name << prefix << ":" << nm;
    std::stringstream value;
    value << val;
    std::string n = name.str();
    add_stat(n.data(), static_cast<uint16_t>(n.length()),
             value.str().data(),
             static_cast<uint32_t>(value.str().length()),
             c);
}

void MCKVStore::optimizeWrites(std::vector<queued_item> &items) {
    if (items.empty()) {
        return;
    }
    CompareQueuedItemsByVBAndKey cq;
    // Make sure that the items are sorted in the ascending order of vbucket ids and keys.
    assert(sorted(items.begin(), items.end(), cq));

    size_t pos = 0;
    uint16_t current_vbid = items[0]->getVBucketId();
    std::vector< std::vector<queued_item> > vb_chunks;
    std::vector<queued_item> chunk;
    std::vector<queued_item>::iterator it = items.begin();
    for (; it != items.end(); ++it) {
        bool moveToNextVB = current_vbid != (*it)->getVBucketId() ? true : false;
        if (!moveToNextVB) {
            chunk.push_back(*it);
        }
        if (chunk.size() == vbBatchSize || moveToNextVB || (it + 1) == items.end()) {
            if (pos < vb_chunks.size()) {
                std::vector<queued_item> &chunk_items = vb_chunks[pos];
                chunk_items.insert(chunk_items.end(), chunk.begin(), chunk.end());
            } else {
                vb_chunks.push_back(chunk);
            }
            chunk.clear();
            if (moveToNextVB) {
                chunk.push_back(*it);
                current_vbid = (*it)->getVBucketId();
                pos = 0;
            } else {
                ++pos;
            }
        }
    }
    if (!chunk.empty()) {
        assert(pos < vb_chunks.size());
        std::vector<queued_item> &chunk_items = vb_chunks[pos];
        chunk_items.insert(chunk_items.end(), chunk.begin(), chunk.end());
    }

    items.clear();
    std::vector< std::vector<queued_item> >::iterator iter = vb_chunks.begin();
    for (; iter != vb_chunks.end(); ++iter) {
        items.insert(items.end(), iter->begin(), iter->end());
    }
}

void MCKVStore::processTxnSizeChange(size_t txn_size) {
    size_t new_batch_size = txn_size / vbBatchCount;
    vbBatchSize = new_batch_size == 0 ? vbBatchSize : new_batch_size;
}

void MCKVStore::setVBBatchCount(size_t batch_count) {
    if (vbBatchCount == batch_count) {
        return;
    }
    vbBatchCount = batch_count;
    size_t new_batch_size = engine.getEpStore()->getTxnSize() / vbBatchCount;
    vbBatchSize = new_batch_size == 0 ? vbBatchSize : new_batch_size;
    mc->setVBucketBatchCount(vbBatchCount, NULL);
}
