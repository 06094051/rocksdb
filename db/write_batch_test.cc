// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/db.h"

#include <memory>
#include "db/skiplistrep.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "leveldb/env.h"
#include "util/logging.h"
#include "util/testharness.h"

namespace leveldb {

static std::string PrintContents(WriteBatch* b) {
  InternalKeyComparator cmp(BytewiseComparator());
  auto factory = std::make_shared<SkipListFactory>();
  MemTable* mem = new MemTable(cmp, factory);
  mem->Ref();
  std::string state;
  Status s = WriteBatchInternal::InsertInto(b, mem);
  int count = 0;
  Iterator* iter = mem->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey ikey;
    memset((void *)&ikey, 0, sizeof(ikey));
    ASSERT_TRUE(ParseInternalKey(iter->key(), &ikey));
    switch (ikey.type) {
      case kTypeValue:
        state.append("Put(");
        state.append(ikey.user_key.ToString());
        state.append(", ");
        state.append(iter->value().ToString());
        state.append(")");
        count++;
        break;
      case kTypeMerge:
        state.append("Merge(");
        state.append(ikey.user_key.ToString());
        state.append(", ");
        state.append(iter->value().ToString());
        state.append(")");
        count++;
        break;
      case kTypeDeletion:
        state.append("Delete(");
        state.append(ikey.user_key.ToString());
        state.append(")");
        count++;
        break;
      case kTypeLogData:
        assert(false);
        break;
    }
    state.append("@");
    state.append(NumberToString(ikey.sequence));
  }
  delete iter;
  if (!s.ok()) {
    state.append(s.ToString());
  } else if (count != WriteBatchInternal::Count(b)) {
    state.append("CountMismatch()");
  }
  mem->Unref();
  return state;
}

class WriteBatchTest { };

TEST(WriteBatchTest, Empty) {
  WriteBatch batch;
  ASSERT_EQ("", PrintContents(&batch));
  ASSERT_EQ(0, WriteBatchInternal::Count(&batch));
  ASSERT_EQ(0, batch.Count());
}

TEST(WriteBatchTest, Multiple) {
  WriteBatch batch;
  batch.Put(Slice("foo"), Slice("bar"));
  batch.Delete(Slice("box"));
  batch.Put(Slice("baz"), Slice("boo"));
  WriteBatchInternal::SetSequence(&batch, 100);
  ASSERT_EQ(100U, WriteBatchInternal::Sequence(&batch));
  ASSERT_EQ(3, WriteBatchInternal::Count(&batch));
  ASSERT_EQ("Put(baz, boo)@102"
            "Delete(box)@101"
            "Put(foo, bar)@100",
            PrintContents(&batch));
  ASSERT_EQ(3, batch.Count());
}

TEST(WriteBatchTest, Corruption) {
  WriteBatch batch;
  batch.Put(Slice("foo"), Slice("bar"));
  batch.Delete(Slice("box"));
  WriteBatchInternal::SetSequence(&batch, 200);
  Slice contents = WriteBatchInternal::Contents(&batch);
  WriteBatchInternal::SetContents(&batch,
                                  Slice(contents.data(),contents.size()-1));
  ASSERT_EQ("Put(foo, bar)@200"
            "Corruption: bad WriteBatch Delete",
            PrintContents(&batch));
}

TEST(WriteBatchTest, Append) {
  WriteBatch b1, b2;
  WriteBatchInternal::SetSequence(&b1, 200);
  WriteBatchInternal::SetSequence(&b2, 300);
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("",
            PrintContents(&b1));
  ASSERT_EQ(0, b1.Count());
  b2.Put("a", "va");
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("Put(a, va)@200",
            PrintContents(&b1));
  ASSERT_EQ(1, b1.Count());
  b2.Clear();
  b2.Put("b", "vb");
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("Put(a, va)@200"
            "Put(b, vb)@201",
            PrintContents(&b1));
  ASSERT_EQ(2, b1.Count());
  b2.Delete("foo");
  WriteBatchInternal::Append(&b1, &b2);
  ASSERT_EQ("Put(a, va)@200"
            "Put(b, vb)@202"
            "Put(b, vb)@201"
            "Delete(foo)@203",
            PrintContents(&b1));
  ASSERT_EQ(4, b1.Count());
}

TEST(WriteBatchTest, Blob) {
  WriteBatch batch;
  batch.Put(Slice("k1"), Slice("v1"));
  batch.Put(Slice("k2"), Slice("v2"));
  batch.Put(Slice("k3"), Slice("v3"));
  batch.PutLogData(Slice("blob1"));
  batch.Delete(Slice("k2"));
  batch.PutLogData(Slice("blob2"));
  batch.Merge(Slice("foo"), Slice("bar"));
  ASSERT_EQ(5, batch.Count());
  ASSERT_EQ("Merge(foo, bar)@4"
            "Put(k1, v1)@0"
            "Delete(k2)@3"
            "Put(k2, v2)@1"
            "Put(k3, v3)@2",
            PrintContents(&batch));

  struct Handler : public WriteBatch::Handler {
    std::string seen;
    virtual void Put(const Slice& key, const Slice& value) {
      seen += "Put(" + key.ToString() + ", " + value.ToString() + ")";
    }
    virtual void Merge(const Slice& key, const Slice& value) {
      seen += "Merge(" + key.ToString() + ", " + value.ToString() + ")";
    }
    virtual void LogData(const Slice& blob) {
      seen += "LogData(" + blob.ToString() + ")";
    }
    virtual void Delete(const Slice& key) {
      seen += "Delete(" + key.ToString() + ")";
    }
  } handler;
  batch.Iterate(&handler);
  ASSERT_EQ(
            "Put(k1, v1)"
            "Put(k2, v2)"
            "Put(k3, v3)"
            "LogData(blob1)"
            "Delete(k2)"
            "LogData(blob2)"
            "Merge(foo, bar)",
            handler.seen);
}

}  // namespace leveldb

int main(int argc, char** argv) {
  return leveldb::test::RunAllTests();
}
