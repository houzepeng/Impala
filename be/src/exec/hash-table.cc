// Copyright (c) 2011 Cloudera, Inc. All rights reserved.

#include "exec/hash-table.h"

#include <boost/functional/hash.hpp>
#include <glog/logging.h>
#include <sstream>

#include "exprs/expr.h"
#include "runtime/descriptors.h"
#include "runtime/raw-value.h"
#include "runtime/tuple-row.h"
#include "util/debug-util.h"

using namespace std;
using namespace boost;

namespace impala {

HashTable::HashTable(const std::vector<Expr*>& build_exprs,
                     const std::vector<Expr*>& probe_exprs,
                     const RowDescriptor& build_row_desc,
                     bool stores_nulls)
  : hash_fn_(this),
    equals_fn_(this),
    // TODO: how many buckets?
    // for now, we just pick some arbitrary prime number, but this should
    // ideally be informed by the estimated size of the final table
    // (but our planner isn't good enough yet to produce that estimate)
    hash_tbl_(new HashSet(1031, hash_fn_, equals_fn_)),
    build_exprs_(build_exprs),
    probe_exprs_(probe_exprs),
    build_row_desc_(build_row_desc),
    current_probe_row_(NULL),
    stores_nulls_(stores_nulls) {
}

size_t HashTable::HashFn::operator()(TupleRow* const& r) const {
  const vector<Expr*>* exprs = NULL;
  TupleRow* row = NULL;
  if (r == NULL) {
    DCHECK(hash_tbl_->current_probe_row_ != NULL);
    // compute hash value from probe_exprs_ and current_probe_row_
    row = hash_tbl_->current_probe_row_;
    exprs = &hash_tbl_->probe_exprs_;
  } else {
    // evaluate build_exprs_ against r
    row = r;
    exprs = &hash_tbl_->build_exprs_;
  }

  size_t seed = 0;
  for (int i = 0; i < exprs->size(); ++i) {
    const void* value;
    Expr* expr = (*exprs)[i];
    value = expr->GetValue(row);
    DCHECK(hash_tbl_->stores_nulls_ || r == NULL || value != NULL);
    // don't ignore NULLs; we want (1, NULL) to return a different hash
    // value than (NULL, 1)
    size_t hash_value =
        (value == NULL ? 0 : RawValue::GetHashValue(value, expr->type()));
    hash_combine(seed, hash_value);
  }
  return seed;
}

bool HashTable::EqualsFn::operator()(
    TupleRow* const& r1, TupleRow* const& r2) const {
  const vector<Expr*>* r1_exprs = NULL;
  TupleRow* r1_row = NULL;
  if (r1 == NULL) {
    DCHECK(hash_tbl_->current_probe_row_ != NULL);
    // compute r1's value from probe_exprs_ and current_probe_row_
    r1_row = hash_tbl_->current_probe_row_;
    r1_exprs = &hash_tbl_->probe_exprs_;
  } else {
    // evaluate build_exprs_ against r1
    r1_row = r1;
    r1_exprs = &hash_tbl_->build_exprs_;
  }

  // r2 is always non-NULL and is a resident row.
  DCHECK(r2 != NULL);

  for (int i = 0; i < hash_tbl_->build_exprs_.size(); ++i) {
    const void* value1 = (*r1_exprs)[i]->GetValue(r1_row);
    const void* value2 = hash_tbl_->build_exprs_[i]->GetValue(r2);

    DCHECK_EQ((*r1_exprs)[i]->type(), hash_tbl_->build_exprs_[i]->type());
    if (value1 == NULL || value2 == NULL) {
      // if nulls are stored, they are always considered not-equal;
      // if they are stored, we pretend NULL == NULL
      if (!hash_tbl_->stores_nulls_ || value1 != NULL || value2 != NULL) return false;
    } else {
      if (RawValue::Compare(value1, value2, (*r1_exprs)[i]->type()) != 0) return false;
    }
  }
  return true;
} 

void HashTable::Insert(TupleRow* r) {
  if (!stores_nulls_) {
    // check for nulls
    for (int i = 0; i < build_exprs_.size(); ++i) {
      if (build_exprs_[i]->GetValue(r) == NULL) return;
    }
  }
  hash_tbl_->insert(r);
}

void HashTable::Scan(TupleRow* probe_row, Iterator* it) {
  current_probe_row_ = probe_row;
  if (probe_row != NULL) {
    // returns rows that are equal to current_probe_row_.
    it->Reset(hash_tbl_->equal_range(NULL));
  } else {
    // return all rows
    it->Reset(make_pair(hash_tbl_->begin(), hash_tbl_->end()));
  }
}

void HashTable::DebugString(int indentation_level, std::stringstream* out) const {
  *out << string(indentation_level * 2, ' ');
  *out << "HashTbl(stores_nulls=" << (stores_nulls_ ? "true" : "false")
       << " build_exprs=" << Expr::DebugString(build_exprs_)
       << " probe_exprs=" << Expr::DebugString(probe_exprs_);
  *out << ")";
}

string HashTable::DebugString() {
  stringstream out;
  out << "size=" << hash_tbl_->size() << "\n";
  Iterator i;
  Scan(NULL, &i);
  TupleRow* r;
  while ((r = i.GetNext()) != NULL) {
    out << "row " << r << ": " << PrintRow(r, build_row_desc_) << "\n";
  }
  return out.str();
}

}
