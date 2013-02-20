#include <leveldb/db.h>

#include <google/protobuf/message.h>

#include <string>
#include <vector>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "logging/logging.hpp"

#include "messages/state.hpp"

#include "state/leveldb.hpp"
#include "state/state.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace state {

LevelDBStateProcess::LevelDBStateProcess(const string& _path)
  : path(_path), db(NULL) {}


LevelDBStateProcess::~LevelDBStateProcess()
{
  delete db; // Might be null if open failed in LevelDBStateProcess::initialize.
}


void LevelDBStateProcess::initialize()
{
  leveldb::Options options;
  options.create_if_missing = true;

  leveldb::Status status = leveldb::DB::Open(options, path, &db);

  if (!status.ok()) {
    // TODO(benh): Consider trying to repair the DB.
    error = Option<string>::some(status.ToString());
  }

  // TODO(benh): Conditionally compact to avoid long recovery times?
  db->CompactRange(NULL, NULL);
}


Future<vector<string> > LevelDBStateProcess::names()
{
  if (error.isSome()) {
    return Future<vector<string> >::failed(error.get());
  }

  vector<string> results;

  leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());

  iterator->SeekToFirst();

  while (iterator->Valid()) {
    results.push_back(iterator->key().ToString());
    iterator->Next();
  }

  delete iterator;

  return results;
}


Future<Option<Entry> > LevelDBStateProcess::fetch(const string& name)
{
  if (error.isSome()) {
    return Future<Option<Entry> >::failed(error.get());
  }

  Try<Option<Entry> > option = get(name);

  if (option.isError()) {
    return Future<Option<Entry> >::failed(option.error());
  }

  return option.get();
}


Future<bool> LevelDBStateProcess::swap(const Entry& entry, const UUID& uuid)
{
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  }

  // We do a fetch first to make sure the version has not changed. This
  // could be optimized in the future, for now it will probably hit
  // the cache anyway.
  Try<Option<Entry> > option = get(entry.name());

  if (option.isError()) {
    return Future<bool>::failed(option.error());
  }

  if (option.get().isSome()) {
    if (UUID::fromBytes(option.get().get().uuid()) != uuid) {
      return false;
    }
  }

  // Note that there is no need to do the DB::Get and DB::Put
  // "atomically" because only one db can be opened at a time, so
  // there can not be any writes that occur concurrently.

  Try<bool> result = put(entry);

  if (result.isError()) {
    return Future<bool>::failed(result.error());
  }

  return result.get();
}


Try<Option<Entry> > LevelDBStateProcess::get(const string& name)
{
  CHECK(error.isNone());

  leveldb::ReadOptions options;

  string value;

  leveldb::Status status = db->Get(options, name, &value);

  if (status.IsNotFound()) {
    return None();
  } else if (!status.ok()) {
    return Error(status.ToString());
  }

  google::protobuf::io::ArrayInputStream stream(value.data(), value.size());

  Entry entry;

  if (!entry.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize Entry");
  }

  return Option<Entry>::some(entry);
}


Try<bool> LevelDBStateProcess::put(const Entry& entry)
{
  CHECK(error.isNone());

  leveldb::WriteOptions options;
  options.sync = true;

  string value;

  if (!entry.SerializeToString(&value)) {
    return Error("Failed to serialize Entry");
  }

  leveldb::Status status = db->Put(options, entry.name(), value);

  if (!status.ok()) {
    return Error(status.ToString());
  }

  return true;
}

} // namespace state {
} // namespace internal {
} // namespace mesos {
