/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include "vbucket.hh"
#include "ep_engine.h"
#include "ep.hh"
#include "backfill.hh"


static bool isMemoryUsageTooHigh(EPStats &stats) {
    double currentSize = static_cast<double>(stats.currentSize.get() + stats.memOverhead.get());
    double maxSize = static_cast<double>(stats.maxDataSize.get());
    return currentSize > (maxSize * BACKFILL_MEM_THRESHOLD);
}

/**
 * Callback class used to process an item backfilled from disk and push it into
 * the corresponding TAP queue.
 */
class BackfillDiskCallback : public Callback<GetValue> {
public:
    BackfillDiskCallback(const std::string &n, TapConnMap &tcm, EventuallyPersistentEngine* e)
        : tapConnName(n), connMap(tcm), engine(e) {
        assert(engine);
    }

    void callback(GetValue &val);

private:

    const std::string           tapConnName;
    TapConnMap                 &connMap;
    EventuallyPersistentEngine *engine;
};

void BackfillDiskCallback::callback(GetValue &gv) {
    ReceivedItemTapOperation tapop(true);
    // if the tap connection is closed, then free an Item instance
    if (!connMap.performTapOp(tapConnName, tapop, gv.getValue())) {
        delete gv.getValue();
    }

    NotifyPausedTapOperation notifyOp;
    connMap.performTapOp(tapConnName, notifyOp, engine);
}

bool BackfillDiskLoad::callback(Dispatcher &d, TaskId t) {
    bool valid = false;

    if (isMemoryUsageTooHigh(engine->getEpStats())) {
         d.snooze(t, 1);
         return true;
    }

    if (connMap.checkConnectivity(name) && !engine->getEpStore()->isFlushAllScheduled()) {
        shared_ptr<Callback<GetValue> > backfill_cb(new BackfillDiskCallback(name, connMap, engine));
        store->dump(vbucket, backfill_cb);
        valid = true;
    }
    // Should decr the disk backfill counter regardless of the connectivity status
    CompleteDiskBackfillTapOperation op;
    connMap.performTapOp(name, op, static_cast<void*>(NULL));

    if (valid && connMap.checkBackfillCompletion(name)) {
        engine->notifyNotificationThread();
    }

    return false;
}

std::string BackfillDiskLoad::description() {
    std::stringstream rv;
    rv << "Loading TAP backfill from disk for vb " << vbucket;
    return rv.str();
}

bool BackFillVisitor::visitBucket(RCPtr<VBucket> &vb) {
    apply();

    if (vBucketFilter(vb->getId())) {
        VBucketVisitor::visitBucket(vb);
        // If the current resident ratio for a given vbucket is below the resident threshold
        // for memory backfill only, schedule the disk backfill for more efficient bg fetches.
        double numItems = static_cast<double>(vb->ht.getNumItems());
        double numNonResident = static_cast<double>(vb->ht.getNumNonResidentItems());
        if (numItems == 0) {
            return true;
        }

        double residentThreshold = engine->getTapConfig().getBackfillResidentThreshold();
        residentRatioBelowThreshold =
            ((numItems - numNonResident) / numItems) < residentThreshold ? true : false;
        if (efficientVBDump && residentRatioBelowThreshold) {
            vbuckets.push_back(vb->getId());
            ScheduleDiskBackfillTapOperation tapop;
            engine->tapConnMap->performTapOp(name, tapop, static_cast<void*>(NULL));
        }
        // When the backfill is scheduled for a given vbucket, set the TAP cursor to
        // the beginning of the open checkpoint.
        engine->tapConnMap->SetCursorToOpenCheckpoint(name, vb->getId());
        return true;
    }
    return false;
}

void BackFillVisitor::visit(StoredValue *v) {
    // If efficient VBdump is supported and an item is not resident,
    // skip the item as it will be fetched by the disk backfill.
    if (efficientVBDump && residentRatioBelowThreshold && !v->isResident()) {
        return;
    }
    std::string k = v->getKey();
    queued_item qi(new QueuedItem(k, value_t(NULL),
                                  currentBucket->getId(), queue_op_set,
                                  -1, v->getId()));
    uint16_t shardId = engine->kvstore->getShardId(*qi);
    found.push_back(std::make_pair(shardId, qi));
}

void BackFillVisitor::apply(void) {
    // If efficient VBdump is supported, schedule all the disk backfill tasks.
    if (efficientVBDump) {
        std::vector<uint16_t>::iterator it = vbuckets.begin();
        for (; it != vbuckets.end(); it++) {
            Dispatcher *d(engine->epstore->getRODispatcher());
            KVStore *underlying(engine->epstore->getROUnderlying());
            assert(d);
            shared_ptr<DispatcherCallback> cb(new BackfillDiskLoad(name,
                                                                   engine,
                                                                   *engine->tapConnMap,
                                                                   underlying,
                                                                   *it,
                                                                   validityToken));
            d->schedule(cb, NULL, Priority::TapBgFetcherPriority);
        }
        vbuckets.clear();
    }

    setEvents();
}

void BackFillVisitor::setEvents() {
    if (checkValidity()) {
        if (!found.empty()) {
            // Don't notify unless we've got some data..
            TaggedQueuedItemComparator<uint16_t> comparator;
            std::sort(found.begin(), found.end(), comparator);

            std::vector<std::pair<uint16_t, queued_item> >::iterator it(found.begin());
            for (; it != found.end(); ++it) {
                queue->push_back(it->second);
            }
            found.clear();
            engine->tapConnMap->setEvents(name, queue);
        }
    }
}

bool BackFillVisitor::pauseVisitor() {
    bool pause(true);

    ssize_t theSize(engine->tapConnMap->backfillQueueDepth(name));
    if (!checkValidity() || theSize < 0) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "TapProducer %s went away.  Stopping backfill.\n",
                         name.c_str());
        valid = false;
        return false;
    }

    ssize_t maxBackfillSize = engine->getTapConfig().getBackfillBacklogLimit();
    pause = theSize > maxBackfillSize || isMemoryUsageTooHigh(engine->getEpStats());

    if (pause) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Tap queue depth too big for %s or memory usage too high, sleeping\n",
                         name.c_str());
    }
    return pause;
}

void BackFillVisitor::complete() {
    apply();
    CompleteBackfillTapOperation tapop;
    engine->tapConnMap->performTapOp(name, tapop, static_cast<void*>(NULL));
    if (engine->tapConnMap->checkBackfillCompletion(name)) {
        engine->notifyNotificationThread();
    }
}

bool BackFillVisitor::checkValidity() {
    if (valid) {
        valid = engine->tapConnMap->checkConnectivity(name);
        if (!valid) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Backfilling connectivity for %s went invalid. Stopping backfill.\n",
                             name.c_str());
        }
    }
    return valid;
}

bool BackfillTask::callback(Dispatcher &d, TaskId t) {
    (void) t;
    epstore->visit(bfv, "Backfill task", &d, Priority::BackfillTaskPriority, true, 1);
    return false;
}
