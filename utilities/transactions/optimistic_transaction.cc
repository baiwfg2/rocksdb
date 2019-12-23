//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "utilities/transactions/optimistic_transaction.h"

#include <string>

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "rocksdb/comparator.h"
#include "rocksdb/db.h"
#include "rocksdb/status.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"
#include "util/cast_util.h"
#include "util/string_util.h"
#include "utilities/transactions/transaction_util.h"
#include "utilities/transactions/optimistic_transaction.h"
#include "utilities/transactions/optimistic_transaction_db_impl.h"

namespace rocksdb {

struct WriteOptions;

OptimisticTransaction::OptimisticTransaction(
    OptimisticTransactionDB* txn_db, const WriteOptions& write_options,
    const OptimisticTransactionOptions& txn_options)
    : TransactionBaseImpl(txn_db->GetBaseDB(), write_options), txn_db_(txn_db) {
  Initialize(txn_options);
}

void OptimisticTransaction::Initialize(
    const OptimisticTransactionOptions& txn_options) {
  if (txn_options.set_snapshot) {
    SetSnapshot();
  }
}

void OptimisticTransaction::Reinitialize(
    OptimisticTransactionDB* txn_db, const WriteOptions& write_options,
    const OptimisticTransactionOptions& txn_options) {
  TransactionBaseImpl::Reinitialize(txn_db->GetBaseDB(), write_options);
  Initialize(txn_options);
}

OptimisticTransaction::~OptimisticTransaction() {}

void OptimisticTransaction::Clear() { TransactionBaseImpl::Clear(); }

Status OptimisticTransaction::Prepare() {
  return Status::InvalidArgument(
      "Two phase commit not supported for optimistic transactions.");
}

Status OptimisticTransaction::Commit() {
  auto txn_db_impl = static_cast_with_check<OptimisticTransactionDBImpl,
                                            OptimisticTransactionDB>(txn_db_);
  assert(txn_db_impl);
  OccValidationPolicy policy = txn_db_impl->GetValidatePolicy();
  if (policy == OccValidationPolicy::VALIDATE_PARALLEL) {
    return CommitWithParallelValidate();
  } else if (policy == OccValidationPolicy::VALIDATE_SERIAL) {
    return CommitWithSerialValidate();
  } else {
    assert(0);
  }
  // unreachable, just void compiler complain
  return Status::OK();
}

Status OptimisticTransaction::CommitWithSerialValidate() {
  // Set up callback which will call CheckTransactionForConflicts() to
  // check whether this transaction is safe to be committed.
  OptimisticTransactionCallback callback(this);

  DBImpl* db_impl = static_cast_with_check<DBImpl, DB>(db_->GetRootDB());

  Status s = db_impl->WriteWithCallback(
      write_options_, GetWriteBatch()->GetWriteBatch(), &callback);

  if (s.ok()) {
    Clear();
  }

  return s;
}

Status OptimisticTransaction::CommitWithParallelValidate() {
  auto txn_db_impl = static_cast_with_check<OptimisticTransactionDBImpl,
                                            OptimisticTransactionDB>(txn_db_);
  assert(txn_db_impl);
  DBImpl* db_impl = static_cast_with_check<DBImpl, DB>(db_->GetRootDB());
  assert(db_impl);
  const size_t space = txn_db_impl->GetLockBucketsSize();
  std::set<size_t> lk_idxes;
  std::vector<std::unique_lock<std::mutex>> lks;
  for (auto& cfit : GetTrackedKeys()) {
    for (auto& keyit : cfit.second) {
      lk_idxes.insert(fastrange64(GetSliceNPHash64(keyit.first), space));
    }
  }
  for (auto v : lk_idxes) {
    lks.emplace_back(txn_db_impl->LockBucket(v));
  }

  Status s = TransactionUtil::CheckKeysForConflicts(db_impl, GetTrackedKeys(),
                                                    true /* cache_only */);
  if (!s.ok()) {
    return s;
  }

  s = db_impl->Write(write_options_, GetWriteBatch()->GetWriteBatch());
  if (s.ok()) {
    Clear();
  }

  return s;
}

Status OptimisticTransaction::Rollback() {
  Clear();
  return Status::OK();
}

// Record this key so that we can check it for conflicts at commit time.
//
// 'exclusive' is unused for OptimisticTransaction.
Status OptimisticTransaction::TryLock(ColumnFamilyHandle* column_family,
                                      const Slice& key, bool read_only,
                                      bool exclusive, const bool do_validate,
                                      const bool assume_tracked) {
  assert(!assume_tracked);  // not supported
  (void)assume_tracked;
  if (!do_validate) {
    return Status::OK();
  }
  uint32_t cfh_id = GetColumnFamilyID(column_family);

  SetSnapshotIfNeeded();

  SequenceNumber seq;
  if (snapshot_) {
    seq = snapshot_->GetSequenceNumber();
  } else {
    seq = db_->GetLatestSequenceNumber();
  }

  std::string key_str = key.ToString();

  TrackKey(cfh_id, key_str, seq, read_only, exclusive);

  // Always return OK. Confilct checking will happen at commit time.
  return Status::OK();
}

// Returns OK if it is safe to commit this transaction.  Returns Status::Busy
// if there are read or write conflicts that would prevent us from committing OR
// if we can not determine whether there would be any such conflicts.
//
// Should only be called on writer thread in order to avoid any race conditions
// in detecting write conflicts.
Status OptimisticTransaction::CheckTransactionForConflicts(DB* db) {
  Status result;

  auto db_impl = static_cast_with_check<DBImpl, DB>(db);

  // Since we are on the write thread and do not want to block other writers,
  // we will do a cache-only conflict check.  This can result in TryAgain
  // getting returned if there is not sufficient memtable history to check
  // for conflicts.
  return TransactionUtil::CheckKeysForConflicts(db_impl, GetTrackedKeys(),
                                                true /* cache_only */);
}

Status OptimisticTransaction::SetName(const TransactionName& /* unused */) {
  return Status::InvalidArgument("Optimistic transactions cannot be named.");
}

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
