#ifndef MINIDB_H
#define MINIDB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DB_OK 0
#define DB_NOT_FOUND 1
#define DB_ERR (-1)

typedef struct db_handle minidb;

typedef struct minidb_options {
  size_t memtable_size_bytes;
  int sync_on_write;
  int compaction_trigger_files;
  size_t block_size_bytes;
  int bloom_bits_per_key;
  size_t level_base_size_bytes;
  int level_size_multiplier;
  size_t target_sstable_size_bytes;
} minidb_options;

typedef struct minidb_stats {
  unsigned long long memtable_entries;
  unsigned long long immutable_entries;
  unsigned long long sstable_count;
  unsigned long long sstable_entries;
  unsigned long long wal_bytes;
  unsigned long long last_sequence;
  unsigned long long flush_count;
  unsigned long long compaction_count;
  unsigned long long get_count;
  unsigned long long put_count;
  unsigned long long delete_count;
  unsigned long long level_files[4];
  unsigned long long level_bytes[4];
} minidb_stats;

void db_options_init(minidb_options* options);
minidb* db_open(const char* path, const minidb_options* options);
int db_put(minidb* db, const char* key, size_t key_len, const char* value, size_t value_len);
int db_get(minidb* db, const char* key, size_t key_len, char** value, size_t* value_len);
void db_free_value(char* value);
int db_delete(minidb* db, const char* key, size_t key_len);
int db_flush(minidb* db);
int db_compact(minidb* db);
int db_verify_levels(minidb* db);
int db_levels_text(minidb* db, char** text, size_t* text_len);
void db_get_stats(minidb* db, minidb_stats* stats);
const char* db_last_error(minidb* db);
int db_close(minidb* db);

#ifdef __cplusplus
}
#endif

#endif
