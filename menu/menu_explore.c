/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2020 - Daniel De Matteis
 *  Copyright (C) 2020      - Psyraven
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include "menu_driver.h"
#include "menu_cbs.h"
#include "../retroarch.h"
#include "../configuration.h"
#include "../playlist.h"
#include "../libretro-db/libretrodb.h"
#include <compat/strcasestr.h>
#include <compat/strl.h>
#include <array/rbuf.h>

#define EX_ARENA_ALIGNMENT 8
#define EX_ARENA_BLOCK_SIZE (64 * 1024)
#define EX_ARENA_ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

/* Explore */
enum
{
   EXPLORE_BY_DEVELOPER          = 0,
   EXPLORE_BY_PUBLISHER,
   EXPLORE_BY_RELEASEYEAR,
   EXPLORE_BY_PLAYERCOUNT,
   EXPLORE_BY_GENRE,
   EXPLORE_BY_ORIGIN,
   EXPLORE_BY_REGION,
   EXPLORE_BY_FRANCHISE,
   EXPLORE_BY_TAGS,
   EXPLORE_BY_SYSTEM,
   EXPLORE_CAT_COUNT,

   EXPLORE_ICONS_OFF             = 0,
   EXPLORE_ICONS_CONTENT         = 1,
   EXPLORE_ICONS_SYSTEM_CATEGORY = 2,

   EXPLORE_TYPE_ADDITIONALFILTER = FILE_TYPE_RDB, /* database icon */
   EXPLORE_TYPE_FILTERNULL       = MENU_SETTINGS_LAST,
   EXPLORE_TYPE_SEARCH,
   EXPLORE_TYPE_SHOWALL,
   EXPLORE_TYPE_FIRSTCATEGORY,
   EXPLORE_TYPE_FIRSTITEM        = EXPLORE_TYPE_FIRSTCATEGORY + EXPLORE_CAT_COUNT
};

/* Arena allocator */
typedef struct ex_arena
{
   char *ptr;
   char *end;
   char **blocks;
} ex_arena;

typedef struct ex_hashmap32
{
   uint32_t len;
   uint32_t cap;
   uint32_t *keys;
   uintptr_t *vals;
} ex_hashmap32;

typedef struct
{
   uint32_t idx;
   char str[1];
} explore_string_t;

typedef struct
{
   const struct playlist_entry* playlist_entry;
   explore_string_t *by[EXPLORE_CAT_COUNT];
   explore_string_t **split;
#ifdef EXPLORE_SHOW_ORIGINAL_TITLE
   char* original_title;
#endif
} explore_entry_t;

typedef struct 
{
   ex_arena arena;
   explore_string_t **by[EXPLORE_CAT_COUNT];
   bool has_unknown[EXPLORE_CAT_COUNT];

   explore_entry_t* entries;
   playlist_t **playlists;
   uintptr_t* icons;
   const char* label_explore_item_str;
   char title[1024];
   char find_string[1024];
   unsigned top_depth;
   unsigned show_icons;
} explore_state_t;

static const struct
{
   enum msg_hash_enums name_enum, by_enum;
   const char* rdbkey;
   bool use_split, is_company, is_numeric;
}
explore_by_info[EXPLORE_CAT_COUNT] = 
{
   { MENU_ENUM_LABEL_VALUE_RDB_ENTRY_DEVELOPER,           MENU_ENUM_LABEL_VALUE_EXPLORE_BY_DEVELOPER,    "developer",   true,  true,  false },
   { MENU_ENUM_LABEL_VALUE_RDB_ENTRY_PUBLISHER,           MENU_ENUM_LABEL_VALUE_EXPLORE_BY_PUBLISHER,    "publisher",   true,  true,  false },
   { MENU_ENUM_LABEL_VALUE_EXPLORE_CATEGORY_RELEASE_YEAR, MENU_ENUM_LABEL_VALUE_EXPLORE_BY_RELEASE_YEAR, "releaseyear", false, false, true  },
   { MENU_ENUM_LABEL_VALUE_EXPLORE_CATEGORY_PLAYER_COUNT, MENU_ENUM_LABEL_VALUE_EXPLORE_BY_PLAYER_COUNT, "users",       false, false, true  },
   { MENU_ENUM_LABEL_VALUE_RDB_ENTRY_GENRE,               MENU_ENUM_LABEL_VALUE_EXPLORE_BY_GENRE,        "genre",       true,  false, false },
   { MENU_ENUM_LABEL_VALUE_RDB_ENTRY_ORIGIN,              MENU_ENUM_LABEL_VALUE_EXPLORE_BY_ORIGIN,       "origin",      false, false, false },
   { MENU_ENUM_LABEL_VALUE_EXPLORE_CATEGORY_REGION,       MENU_ENUM_LABEL_VALUE_EXPLORE_BY_REGION,       "region",      false, false, false },
   { MENU_ENUM_LABEL_VALUE_RDB_ENTRY_FRANCHISE,           MENU_ENUM_LABEL_VALUE_EXPLORE_BY_FRANCHISE,    "franchise",   false, false, false },
   { MENU_ENUM_LABEL_VALUE_EXPLORE_CATEGORY_TAG,          MENU_ENUM_LABEL_VALUE_EXPLORE_BY_TAG,          "tags",        true,  false, false },
   { MENU_ENUM_LABEL_VALUE_CORE_INFO_SYSTEM_NAME,         MENU_ENUM_LABEL_VALUE_EXPLORE_BY_SYSTEM_NAME,  "system",      false, false, false },
};

/* TODO/FIXME - static global */
static explore_state_t* explore_state;

static void ex_arena_grow(ex_arena *arena, size_t min_size)
{
   size_t size = EX_ARENA_ALIGN_UP(
         MAX(min_size, EX_ARENA_BLOCK_SIZE), EX_ARENA_ALIGNMENT);
   arena->ptr  = (char *)malloc(size);
   arena->end  = arena->ptr + size;
   RBUF_PUSH(arena->blocks, arena->ptr);
}

static void *ex_arena_alloc(ex_arena *arena, size_t size)
{
   void *ptr  = NULL;

   if (size > (size_t)(arena->end - arena->ptr))
      ex_arena_grow(arena, size);

   ptr        = arena->ptr;
   arena->ptr = (char *)
      EX_ARENA_ALIGN_UP((uintptr_t)(arena->ptr + size), EX_ARENA_ALIGNMENT);
   return ptr;
}

static void ex_arena_free(ex_arena *arena)
{
   char **it;

   for (it = arena->blocks; it != RBUF_END(arena->blocks); it++)
      free(*it);

   RBUF_FREE(arena->blocks);
   arena->ptr    = NULL;
   arena->end    = NULL;
   arena->blocks = NULL;
}

/* Hash function */
static uint32_t ex_hash32(const char* str)
{
   unsigned char c;
   uint32_t hash = (uint32_t)0x811c9dc5;
   for (; (c = *(str++)) != '\0';)
      hash = ((hash * (uint32_t)0x01000193) ^ (uint32_t)c);
   if (hash)
      return hash;
   return 1;
}

static uint32_t ex_hash32_nocase_filtered(
      const unsigned char* str, size_t len,
      unsigned char f_first, unsigned char f_last)
{
   const unsigned char *end = NULL;
   uint32_t hash            = (uint32_t)0x811c9dc5;
   for (end = str + len; str != end;)
   {
      unsigned char c = *(str++);
      if (c >= f_first && c <= f_last)
         hash = ((hash * (uint32_t)0x01000193) 
               ^ (uint32_t)((c >= 'A' && c <= 'Z') 
                  ? (c | 0x20) : c));
   }
   if (hash)
      return hash;
   return 1;
}

/* Hashmap */
static void ex_hashmap32__grow(ex_hashmap32* map, uint32_t new_cap)
{
   size_t i, j;
   uint32_t old_cap    = map->cap;
   uint32_t *old_keys  = map->keys;
   uintptr_t *old_vals = map->vals;

   map->cap            = (new_cap < 16) ? 16 : new_cap;
   map->keys           = (uint32_t *)calloc(map->cap, sizeof(uint32_t));
   map->vals           = (uintptr_t *)malloc(map->cap * sizeof(uintptr_t));

   for (i = 0; i < old_cap; i++)
   {
      uint32_t key;
      if (!old_keys[i])
         continue;

      for (key = old_keys[i], j = key;; j++)
      {
         if (!map->keys[j &= map->cap - 1])
         {
            map->keys[j] = key;
            map->vals[j] = old_vals[i];
            break;
         }
      }
   }

   free(old_keys);
   free(old_vals);
}

static INLINE void ex_hashmap32_init(ex_hashmap32* map)
{
   map->len = map->cap = 0;
   map->keys = NULL;
   map->vals = NULL;
}

static void ex_hashmap32_free(ex_hashmap32* map)
{
   if (!map)
      return;
   free(map->keys);
   free(map->vals);
}

static uintptr_t ex_hashmap32_getnum(ex_hashmap32* map, uint32_t key)
{
   uint32_t i;
   if (!map || map->len == 0 || !key)
      return 0;
   for (i = key;; i++)
   {
      if (map->keys[i &= map->cap - 1] == key)
         return map->vals[i];
      if (!map->keys[i])
         break;
   }
   return 0;
}

static void ex_hashmap32_setnum(
      ex_hashmap32* map, uint32_t key, uintptr_t val)
{
   uint32_t i;
   if (!key)
      return;
   if (2 * map->len >= map->cap)
      ex_hashmap32__grow(map, 2 * map->cap);

   for (i = key;; i++)
   {
      if (!map->keys[i &= map->cap - 1])
      {
         map->len++;
         map->keys[i] = key;
         map->vals[i] = val;
         return;
      }
      if (map->keys[i] == key)
      {
         map->vals[i] = val;
         return;
      }
   }
}

static INLINE void *ex_hashmap32_getptr(ex_hashmap32* map, uint32_t key)
{
   return (void*)ex_hashmap32_getnum(map, key);
}

static INLINE void ex_hashmap32_setptr(ex_hashmap32* map,
      uint32_t key, void* ptr)
{
   ex_hashmap32_setnum(map, key, (uintptr_t)ptr);
}

static INLINE void *ex_hashmap32_strgetptr(ex_hashmap32* map, const char* str)
{
   return (void*)ex_hashmap32_getnum(map, ex_hash32(str));
}

static INLINE void ex_hashmap32_strsetptr(ex_hashmap32* map,
      const char* str, void* ptr)
{
   ex_hashmap32_setnum(map, ex_hash32(str), (uintptr_t)ptr);
}
static INLINE uintptr_t ex_hashmap32_strgetnum(
      ex_hashmap32* map, const char* str)
{
   return ex_hashmap32_getnum(map, ex_hash32(str));
}
static INLINE void ex_hashmap32_strsetnum(ex_hashmap32* map,
      const char* str, uintptr_t num)
{
   ex_hashmap32_setnum(map, ex_hash32(str), num);
}

static int explore_qsort_func_strings(const void *a_, const void *b_)
{
   const explore_string_t **a = (const explore_string_t**)a_;
   const explore_string_t **b = (const explore_string_t**)b_;
   if ((*a)->str[0] != (*b)->str[0])
      return (unsigned char)(*a)->str[0] - (unsigned char)(*b)->str[0];
   return strcasecmp((*a)->str, (*b)->str);
}

static int explore_qsort_func_entries(const void *a_, const void *b_)
{
   const explore_entry_t *a = (const explore_entry_t*)a_;
   const explore_entry_t *b = (const explore_entry_t*)b_;
   if (a->playlist_entry->label[0] != b->playlist_entry->label[0])
      return (unsigned char)a->playlist_entry->label[0] - (unsigned char)b->playlist_entry->label[0];
   return strcasecmp(a->playlist_entry->label, b->playlist_entry->label);
}

static int explore_qsort_func_menulist(const void *a_, const void *b_)
{
   const struct item_file *a = (const struct item_file*)a_;
   const struct item_file *b = (const struct item_file*)b_;
   if (a->path[0] != b->path[0])
      return (unsigned char)a->path[0] - (unsigned char)b->path[0];
   return strcasecmp(a->path, b->path);
}

static int explore_check_company_suffix(const char* p, bool search_reverse)
{
   if (search_reverse)
   {
      p -= (p[-1] == '.' ? 4 : 3);
      if (p[-1] != ' ')
         return 0;
   }
   if (tolower(p[0]) == 'i' && tolower(p[1]) == 'n' && tolower(p[2]) == 'c')
      return (p[3] == '.' ? 4 : 3); /*, Inc */
   if (tolower(p[0]) == 'l' && tolower(p[1] )== 't' && tolower(p[2]) == 'd')
      return (p[3] == '.' ? 4 : 3); /*, Ltd */
   if (tolower(p[0]) == 't' && tolower(p[1] )== 'h' && tolower(p[2]) == 'e')
      return (p[3] == '.' ? 4 : 3); /*, The */
   return 0;
}

static void explore_add_unique_string(
      explore_state_t *explore,
      ex_hashmap32 *maps, explore_entry_t *e,
      unsigned cat, const char *str,
      explore_string_t ***split_buf)
{
   bool is_company;
   const char *p;
   const char *p_next;
   if (!str || !*str)
   {
      explore->has_unknown[cat] = true;
      return;
   }

   if (!explore_by_info[cat].use_split)
      split_buf = NULL;
   is_company   = explore_by_info[cat].is_company;

   for (p = str + 1;; p++)
   {
      size_t len              = 0;
      uint32_t hash           = 0;
      explore_string_t* entry = NULL;

      if (*p != '/' && *p != ',' && *p != '|' && *p != '\0')
         continue;

      if (!split_buf && *p != '\0')
         continue;

      p_next = p;
      while (*str == ' ')
         str++;
      while (p[-1] == ' ')
         p--;

      if (p == str)
      {
         if (*p == '\0')
            return;
         continue;
      }

      if (is_company && p - str > 5)
      {
         p -= explore_check_company_suffix(p, true);
         while (p[-1] == ' ')
            p--;
      }

      len                     = p - str;
      hash                    = ex_hash32_nocase_filtered(
            (unsigned char*)str, len, '0', 255);
      entry                   = 
         (explore_string_t*)ex_hashmap32_getptr(&maps[cat], hash);

      if (!entry)
      {
         entry                = (explore_string_t*)
            ex_arena_alloc(&explore->arena,
                  sizeof(explore_string_t) + len);
         memcpy(entry->str, str, len);
         entry->str[len]      = '\0';
         RBUF_PUSH(explore->by[cat], entry);
         ex_hashmap32_setptr(&maps[cat], hash, entry);
      }

      if (!e->by[cat])
         e->by[cat] = entry;
      else
         RBUF_PUSH(*split_buf, entry);

      if (*p_next == '\0')
         return;
      if (is_company && *p_next == ',')
      {
         p = p_next + 1;
         while (*p == ' ')
            p++;
         p += explore_check_company_suffix(p, false);
         while (*p == ' ')
            p++;
         if (*p == '\0')
            return;
         if (*p == '/' || *p == ',' || *p == '|')
            p_next = p;
      }
      p = p_next;
      str = p + 1;
   }
}

static void explore_unload_icons(explore_state_t *state)
{
   unsigned i;
   if (!state)
      return;
   for (i = 0; i != RBUF_LEN(state->icons); i++)
      if (state->icons[i])
         video_driver_texture_unload(&state->icons[i]);
}

static void explore_free(explore_state_t *state)
{
   unsigned i;
   if (!state)
      return;
   for (i = 0; i != EXPLORE_CAT_COUNT; i++)
      RBUF_FREE(state->by[i]);

   RBUF_FREE(state->entries);

   for (i = 0; i != RBUF_LEN(state->playlists); i++)
      playlist_free(state->playlists[i]);
   RBUF_FREE(state->playlists);

   explore_unload_icons(state);
   RBUF_FREE(state->icons);

   ex_arena_free(&state->arena);
}

static void explore_load_icons(explore_state_t *state)
{
   char path[PATH_MAX_LENGTH];
   size_t i, pathlen, system_count;
   if (!state)
      return;

   system_count = RBUF_LEN(state->by[EXPLORE_BY_SYSTEM]);

   /* unload any icons that could exist from a previous call to this */
   explore_unload_icons(state);

   /* RBUF_RESIZE leaves memory uninitialised, have to zero it 'manually' */
   RBUF_RESIZE(state->icons, system_count);
   memset(state->icons, 0, RBUF_SIZEOF(state->icons));

   fill_pathname_application_special(path, sizeof(path),
         APPLICATION_SPECIAL_DIRECTORY_ASSETS_SYSICONS);
   if (string_is_empty(path))
      return;

   fill_pathname_slash(path, sizeof(path));
   pathlen = strlen(path);

   for (i = 0; i != system_count; i++)
   {
      struct texture_image ti;

      strlcpy(path + pathlen,
            state->by[EXPLORE_BY_SYSTEM][i]->str, sizeof(path) - pathlen);
      strlcat(path, ".png", sizeof(path));
      if (!path_is_valid(path))
         continue;

      ti.width         = 0;
      ti.height        = 0;
      ti.pixels        = NULL;
      ti.supports_rgba = video_driver_supports_rgba();

      if (!image_texture_load(&ti, path))
         continue;

      if (ti.pixels)
         video_driver_texture_load(&ti,
               TEXTURE_FILTER_MIPMAP_LINEAR, &state->icons[i]);

      image_texture_free(&ti);
   }
}

static explore_state_t *explore_build_list(void)
{
   unsigned i;
   char tmp[PATH_MAX_LENGTH];
   struct explore_rdb
   {
      libretrodb_t *handle;
      size_t count;
      ex_hashmap32 playlist_crcs;
      ex_hashmap32 playlist_names;
      char systemname[256];
   }
   *rdbs                                    = NULL;
   ex_hashmap32 rdb_indices                 = {0};
   ex_hashmap32 cat_maps[EXPLORE_CAT_COUNT] = {{0}};
   explore_string_t **split_buf             = NULL;
   settings_t *settings                     = config_get_ptr();
   const char *directory_playlist           = settings->paths.directory_playlist;
   const char *directory_database           = settings->paths.path_content_database;
   libretro_vfs_implementation_dir *dir     = NULL;

   explore_state_t *explore                 = (explore_state_t*)calloc(
         1, sizeof(*explore));

   if (!explore)
      return NULL;

   explore->label_explore_item_str    = 
      msg_hash_to_str(MENU_ENUM_LABEL_EXPLORE_ITEM);

   /* Index all playlists */
   for (dir = retro_vfs_opendir_impl(directory_playlist, false); dir;)
   {
      playlist_config_t playlist_config;
      size_t j, used_entries                    = 0;
      playlist_t *playlist                      = NULL;
      const char *fext                          = NULL;
      const char *fname                         = NULL;
      uint32_t fhash;

      playlist_config.path[0]                   = '\0';
      playlist_config.base_content_directory[0] = '\0';
      playlist_config.capacity                  = 0;
      playlist_config.old_format                = false;
      playlist_config.compress                  = false;
      playlist_config.fuzzy_archive_match       = false;
      playlist_config.autofix_paths             = false;

      if (!retro_vfs_readdir_impl(dir))
      {
         retro_vfs_closedir_impl(dir);
         break;
      }

      fname                             = retro_vfs_dirent_get_name_impl(dir);
      if (fname)
         fext                           = strrchr(fname, '.');

      if (!fext || strcasecmp(fext, ".lpl"))
         continue;

      fill_pathname_join(playlist_config.path,
            directory_playlist, fname, sizeof(playlist_config.path));
      playlist_config.capacity          = COLLECTION_SIZE;
      playlist                          = playlist_init(&playlist_config);

      fhash = ex_hash32_nocase_filtered(
            (unsigned char*)fname, fext - fname, '0', 255);

      for (j = 0; j < playlist_size(playlist); j++)
      {
         uintptr_t rdb_num;
         uint32_t entry_crc32;
         struct explore_rdb* rdb             = NULL;
         const struct playlist_entry *entry  = NULL;
         const char *db_name                 = fname;
         const char *db_ext                  = fext;
         uint32_t rdb_hash                   = fhash;
         playlist_get_index(playlist, j, &entry);

         /* We also could build label from file name, for now it's required */
         if (!entry->label || !*entry->label)
            continue;

         /* For auto scanned playlists the entry db_name matches the
          * lpl file name and we can just use that */
         if (entry->db_name && *entry->db_name
               && strcasecmp(entry->db_name, fname))
         {
            db_name = entry->db_name;
            db_ext = strrchr(db_name, '.');
            if (!db_ext)
               db_ext = db_name + strlen(db_name);
            rdb_hash = ex_hash32_nocase_filtered(
               (unsigned char*)db_name, db_ext - db_name, '0', 255);
         }

         rdb_num = ex_hashmap32_getnum(&rdb_indices, rdb_hash);
         if (!rdb_num)
         {
            struct explore_rdb newrdb;
            size_t systemname_len;

            newrdb.handle = libretrodb_new();
            newrdb.count  = 0;
            ex_hashmap32_init(&newrdb.playlist_crcs);
            ex_hashmap32_init(&newrdb.playlist_names);

            systemname_len = db_ext - db_name;
            if (systemname_len >= sizeof(newrdb.systemname))
               systemname_len = sizeof(newrdb.systemname)-1;
            memcpy(newrdb.systemname, db_name, systemname_len);
            newrdb.systemname[systemname_len] = '\0';

            fill_pathname_join_noext(
                  tmp, directory_database, db_name, sizeof(tmp));
            strlcat(tmp, ".rdb", sizeof(tmp));

            if (libretrodb_open(tmp, newrdb.handle) != 0)
            {
               /* Invalid RDB file */
               libretrodb_free(newrdb.handle);
               ex_hashmap32_setnum(&rdb_indices, rdb_hash, (uintptr_t)-1);
               continue;
            }

            RBUF_PUSH(rdbs, newrdb);
            rdb_num = (uintptr_t)RBUF_LEN(rdbs);
            ex_hashmap32_setnum(&rdb_indices, rdb_hash, rdb_num);
         }

         if (rdb_num == (uintptr_t)-1)
            continue;

         rdb = &rdbs[rdb_num - 1];
         rdb->count++;
         entry_crc32 = (uint32_t)strtoul(
               (entry->crc32 ? entry->crc32 : ""), NULL, 16);
         if (entry_crc32)
         {
            ex_hashmap32_setptr(&rdb->playlist_crcs,
                  entry_crc32, (void*)entry);
         }
         else
         {
            ex_hashmap32_strsetptr(&rdb->playlist_names,
                  entry->label, (void*)entry);
         }
         used_entries++;
      }

      if (used_entries)
         RBUF_PUSH(explore->playlists, playlist);
      else
         playlist_free(playlist);
   }

   /* Loop through all RDBs referenced in the playlists 
    * and load meta data strings */
   for (i = 0; i != RBUF_LEN(rdbs); i++)
   {
      struct rmsgpack_dom_value item;
      struct explore_rdb* rdb  = &rdbs[i];
      libretrodb_cursor_t *cur = libretrodb_cursor_new();
      bool more                = 
         (
          libretrodb_cursor_open(rdb->handle, cur, NULL) == 0
          && libretrodb_cursor_read_item(cur, &item) == 0);

      for (; more; more = (rmsgpack_dom_value_free(&item),
               libretrodb_cursor_read_item(cur, &item) == 0))
      {
         unsigned k, l, cat;
         explore_entry_t e;
         char *fields[EXPLORE_CAT_COUNT];
         char numeric_buf[EXPLORE_CAT_COUNT][16];
         const struct playlist_entry *entry = NULL;
         uint32_t crc32                     = 0;
         char *name                         = NULL;
#ifdef EXPLORE_SHOW_ORIGINAL_TITLE
         char *original_title               = NULL;
#endif

         if (item.type != RDT_MAP)
            continue;

         for (k = 0; k < EXPLORE_CAT_COUNT; k++)
            fields[k]                       = NULL;

         for (k = 0; k < item.val.map.len; k++)
         {
            const char *key_str             = NULL;
            struct rmsgpack_dom_value *key  = &item.val.map.items[k].key;
            struct rmsgpack_dom_value *val  = &item.val.map.items[k].value;
            if (!key || !val || key->type != RDT_STRING)
               continue;

            key_str                         = key->val.string.buff;
            if (string_is_equal(key_str, "crc"))
            {
               crc32 = swap_if_little32(*(uint32_t*)val->val.binary.buff);
               continue;
            }
            else if (string_is_equal(key_str, "name"))
            {
               name = val->val.string.buff;
               continue;
            }
#ifdef EXPLORE_SHOW_ORIGINAL_TITLE
            else if (string_is_equal(key_str, "original_title"))
            {
               original_title = val->val.string.buff;
               continue;
            }
#endif

            for (cat = 0; cat != EXPLORE_CAT_COUNT; cat++)
            {
               if (!string_is_equal(key_str, explore_by_info[cat].rdbkey))
                  continue;

               if (explore_by_info[cat].is_numeric)
               {
                  if (!val->val.int_)
                     break;
                  snprintf(numeric_buf[cat],
                        sizeof(numeric_buf[cat]),
                        "%d", (int)val->val.int_);
                  fields[cat] = numeric_buf[cat];
                  break;
               }
               if (val->type != RDT_STRING)
                  break;
               fields[cat] = val->val.string.buff;
               break;
            }
         }

         if (crc32)
         {
            entry = (const struct playlist_entry *)ex_hashmap32_getptr(
                  &rdb->playlist_crcs, crc32);
         }
         if (!entry && name)
         {
            entry = (const struct playlist_entry *)ex_hashmap32_strgetptr(
                  &rdb->playlist_names, name);
         }
         if (!entry)
            continue;

         e.playlist_entry  = entry;
         for (l = 0; l < EXPLORE_CAT_COUNT; l++)
            e.by[l]        = NULL;
         e.split           = NULL;
#ifdef EXPLORE_SHOW_ORIGINAL_TITLE
         e.original_title  = NULL;
#endif

         fields[EXPLORE_BY_SYSTEM] = rdb->systemname;

         for (cat = 0; cat != EXPLORE_CAT_COUNT; cat++)
         {
            explore_add_unique_string(explore,
                  cat_maps, &e, cat,
                  fields[cat], &split_buf);
         }

#ifdef EXPLORE_SHOW_ORIGINAL_TITLE
         if (original_title && *original_title)
         {
            size_t len       = strlen(original_title) + 1;
            e.original_title = (char*)
               ex_arena_alloc(&explore->arena, len);
            memcpy(e.original_title, original_title, len);
         }
#endif

         if (RBUF_LEN(split_buf))
         {
            size_t len;

            RBUF_PUSH(split_buf, NULL); /* terminator */
            len        = RBUF_SIZEOF(split_buf);
            e.split    = (explore_string_t **)
               ex_arena_alloc(&explore->arena, len);
            memcpy(e.split, split_buf, len);
            RBUF_CLEAR(split_buf);
         }

         RBUF_PUSH(explore->entries, e);

         /* if all entries have found connections, we can leave early */
         if (--rdb->count == 0)
         {
            rmsgpack_dom_value_free(&item);
            break;
         }
      }

      libretrodb_cursor_close(cur);
      libretrodb_cursor_free(cur);
      libretrodb_close(rdb->handle);
      libretrodb_free(rdb->handle);
      ex_hashmap32_free(&rdb->playlist_crcs);
      ex_hashmap32_free(&rdb->playlist_names);
   }
   RBUF_FREE(split_buf);
   ex_hashmap32_free(&rdb_indices);
   RBUF_FREE(rdbs);

   for (i = 0; i != EXPLORE_CAT_COUNT; i++)
   {
      uint32_t idx;
      size_t len = RBUF_LEN(explore->by[i]);

      if (explore->by[i])
         qsort(explore->by[i], len, sizeof(*explore->by[i]),
               explore_qsort_func_strings);

      for (idx = 0; idx != len; idx++)
         explore->by[i][idx]->idx = idx;

      ex_hashmap32_free(&cat_maps[i]);
   }
   qsort(explore->entries,
         RBUF_LEN(explore->entries),
         sizeof(*explore->entries), explore_qsort_func_entries);
   return explore;
}

static int explore_action_get_title(
      const char *path, const char *label,
      unsigned menu_type, char *s, size_t len)
{
   strlcpy(s, explore_state->title, len);
   return 0;
}

static void explore_append_title(explore_state_t *state,
      const char* fmt, ...)
{
   va_list ap;
   size_t len = strlen(state->title);
   va_start(ap, fmt);
   vsnprintf(state->title + len,
         sizeof(state->title) - len, fmt, ap);
   va_end(ap);
}

static int explore_action_sublabel_spacer(
      file_list_t *list, unsigned type, unsigned i,
      const char *label, const char *path, char *s, size_t len)
{
   const char *menu_driver = menu_driver_ident();

   /* Only add a blank 'spacer' sublabel when
    * using Ozone
    * > In XMB/GLUI it upsets the vertical layout
    * > In RGUI it does nothing other than
    *   unnecessarily blank out the fallback
    *   core title text in the sublabel area */
   if (string_is_equal(menu_driver, "ozone"))
      strlcpy(s, " ", len);

   return 1; /* 1 means it'll never change and can be cached */
}

static int explore_action_ok(const char *path, const char *label,
      unsigned type, size_t idx, size_t entry_idx)
{
   const char* explore_tab = msg_hash_to_str(MENU_ENUM_LABEL_EXPLORE_TAB);
   filebrowser_clear_type();
   return generic_action_ok_displaylist_push(explore_tab,
         NULL, explore_tab, type, idx, entry_idx, ACTION_OK_DL_PUSH_DEFAULT);
}

static menu_file_list_cbs_t *explore_menu_entry(
      file_list_t *list,
      explore_state_t *state,
      const char *path, unsigned type)
{
   menu_file_list_cbs_t *cbs = NULL;
   if (!state)
      return NULL;
   menu_entries_append_enum(list, path,
         state->label_explore_item_str,
         MENU_ENUM_LABEL_EXPLORE_ITEM, type, 0, 0);
   cbs                       = ((menu_file_list_cbs_t*)list->list[list->size-1].actiondata);
   if (!cbs)
      return NULL;
   cbs->action_ok = explore_action_ok;
   return cbs;
}

static void explore_menu_add_spacer(file_list_t *list)
{
   if (list->size)
      ((menu_file_list_cbs_t*)list->list[list->size-1].actiondata)->action_sublabel = explore_action_sublabel_spacer;
}

static void explore_action_find_complete(void *userdata, const char *line)
{
   menu_input_dialog_end();
   if (line && *line)
   {
      strlcpy(explore_state->find_string, line,
            sizeof(explore_state->find_string));
      explore_action_ok(NULL, NULL, EXPLORE_TYPE_SEARCH, 0, 0);
   }
}

static int explore_action_ok_find(const char *path, const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   menu_input_ctx_line_t line;
   line.label                 = msg_hash_to_str(MENU_ENUM_LABEL_VALUE_SEARCH);
   line.label_setting         = NULL;
   line.type                  = 0;
   line.idx                   = 0;
   line.cb                    = explore_action_find_complete;
   menu_input_dialog_start(&line);
   return 0;
}

unsigned menu_displaylist_explore(file_list_t *list)
{
   unsigned i, cat;
   char tmp[512];
   unsigned depth, current_type, current_cat, previous_cat;
   struct item_file *stack_top  = NULL;
   file_list_t *menu_stack      = menu_entries_get_menu_stack_ptr(0);

   if (!explore_state)
   {
      explore_state             = explore_build_list();
      explore_state->top_depth  = (unsigned)menu_stack->size - 1;
      explore_load_icons(explore_state);
   }

   if (menu_stack->size > 1)
   {
      struct item_file *stack   = &menu_stack->list[menu_stack->size - 1];
      menu_file_list_cbs_t* cbs = ((menu_file_list_cbs_t*)stack->actiondata);
      cbs->action_get_title     = explore_action_get_title;
   }

   stack_top                    = menu_stack->list + explore_state->top_depth;
   depth                        = (unsigned)menu_stack->size - 1 - explore_state->top_depth;
   current_type                 = stack_top[depth].type;
   current_cat                  = current_type - EXPLORE_TYPE_FIRSTCATEGORY;
   previous_cat                 = stack_top[depth ? depth - 1 : 0].type - EXPLORE_TYPE_FIRSTCATEGORY;
   explore_state->show_icons    = EXPLORE_ICONS_OFF;

   if (depth)
   {
      unsigned levels           = 0;
      bool clear_find_text      = false;
      ((menu_file_list_cbs_t*)stack_top[depth].actiondata)->action_get_title = 
         explore_action_get_title;

      clear_find_text           = (current_type != EXPLORE_TYPE_SEARCH);
      explore_state->title[0]   = '\0';

      for (i = 1; i < depth; i++)
      {
         unsigned by_selected_type;
         unsigned by_category      = (stack_top[i].type - 
               EXPLORE_TYPE_FIRSTCATEGORY);

         if (stack_top[i].type == EXPLORE_TYPE_SEARCH)
            clear_find_text         = false;
         if (by_category >= EXPLORE_CAT_COUNT)
            continue;

         by_selected_type           = stack_top[i + 1].type;
         explore_append_title(explore_state, "%s%s: %s",
               (levels++ ? " / " : ""),
               msg_hash_to_str(explore_by_info[by_category].name_enum),
               (by_selected_type != EXPLORE_TYPE_FILTERNULL ?
                  explore_state->by[by_category][by_selected_type - EXPLORE_TYPE_FIRSTITEM]->str
                  : msg_hash_to_str(MENU_ENUM_LABEL_VALUE_UNKNOWN)));
      }

      if (clear_find_text)
         explore_state->find_string[0] = '\0';

      if (*explore_state->find_string)
         explore_append_title(explore_state, "%s%s: '%s'",
               (levels++ ? " / " : ""),
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_RDB_ENTRY_NAME),
               explore_state->find_string);
   }

   if (     current_type == MENU_EXPLORE_TAB 
         || current_type == EXPLORE_TYPE_ADDITIONALFILTER)
   {
      /* Explore top or selecting an additional filter */
      bool is_top = (current_type == MENU_EXPLORE_TAB);
      if (is_top)
         strlcpy(explore_state->title,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_TAB),
               sizeof(explore_state->title));
      else
         explore_append_title(explore_state, " - %s",
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_ADDITIONAL_FILTER));

      if (is_top || !*explore_state->find_string)
      {
         menu_file_list_cbs_t *new_cbs = explore_menu_entry(
               list, explore_state,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_SEARCH_NAME),
               EXPLORE_TYPE_SEARCH);
         if (new_cbs)
            new_cbs->action_ok         = explore_action_ok_find;
         explore_menu_add_spacer(list);
      }

      for (cat = 0; cat < EXPLORE_CAT_COUNT; cat++)
      {
         explore_string_t **entries = explore_state->by[cat];
         size_t tmplen;

         if (!RBUF_LEN(entries))
            continue;

         for (i = 1; i < depth; i++)
            if (stack_top[i].type == cat + EXPLORE_TYPE_FIRSTCATEGORY)
               goto SKIP_EXPLORE_BY_CATEGORY;

         tmplen = strlcpy(tmp,
               msg_hash_to_str(explore_by_info[cat].by_enum), sizeof(tmp));

         if (is_top && tmplen < sizeof(tmp) - 5)
         {
            if (explore_by_info[cat].is_numeric)
            {
               snprintf(tmp + tmplen, sizeof(tmp) - tmplen, " (%s - %s)",
                     entries[0]->str, entries[RBUF_LEN(entries) - 1]->str);
            }
            else
            {
               strlcat(tmp, " (", sizeof(tmp));
               snprintf(tmp + tmplen + 2, sizeof(tmp) - tmplen - 2,
                     msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_ITEMS_COUNT),
                     (unsigned)RBUF_LEN(entries));
               strlcat(tmp, ")", sizeof(tmp));
            }
         }

         explore_menu_entry(list, explore_state,
               tmp, cat + EXPLORE_TYPE_FIRSTCATEGORY);

SKIP_EXPLORE_BY_CATEGORY:;
      }

      if (is_top)
      {
         explore_menu_add_spacer(list);
         explore_menu_entry(list, explore_state,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_SHOW_ALL),
               EXPLORE_TYPE_SHOWALL);
      }
   }
   else if (
         depth == 1 
         && current_type != EXPLORE_TYPE_SEARCH 
         && current_type != EXPLORE_TYPE_SHOWALL)
   {
      /* List all items in a selected explore by category */
      explore_string_t **entries = explore_state->by[current_cat];
      unsigned i_last            = RBUF_LEN(entries) - 1;
      for (i = 0; i <= i_last; i++)
         explore_menu_entry(list, explore_state,
               entries[i]->str, EXPLORE_TYPE_FIRSTITEM + i);

      if (explore_state->has_unknown[current_cat])
      {
         explore_menu_add_spacer(list);
         explore_menu_entry(list, explore_state,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_UNKNOWN), EXPLORE_TYPE_FILTERNULL);
      }

      explore_append_title(explore_state,
            msg_hash_to_str(explore_by_info[current_cat].by_enum));

      if (current_cat == EXPLORE_BY_SYSTEM)
         explore_state->show_icons = EXPLORE_ICONS_SYSTEM_CATEGORY;
   }
   else if (
            previous_cat < EXPLORE_CAT_COUNT 
         || current_type < EXPLORE_TYPE_FIRSTITEM)
   {
      bool use_split[10];
      unsigned cats[10];
      explore_string_t* filter[10];
      explore_entry_t *e                  = NULL;
      explore_entry_t *e_end              = NULL;
      ex_hashmap32 map_filtered_category  = {0};
      unsigned levels                     = 0;
      bool use_find                       = (
            *explore_state->find_string != '\0');

      bool is_show_all                    = (depth == 1 && !use_find);
      bool is_filtered_category           = (current_cat < EXPLORE_CAT_COUNT);
      bool filtered_category_have_unknown = false;

      /* List filtered items in a selected explore by category */
      if (is_filtered_category)
      {
         explore_append_title(explore_state, " - %s",
               msg_hash_to_str(explore_by_info[current_cat].by_enum));

        if (current_cat == EXPLORE_BY_SYSTEM)
           explore_state->show_icons = EXPLORE_ICONS_SYSTEM_CATEGORY;
      }
      else
      {
         /* Game list */
         if (is_show_all)
         {
            menu_file_list_cbs_t *new_cbs = NULL;
            explore_append_title(explore_state,
                  msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_ALL));
            new_cbs               = explore_menu_entry(
                  list, explore_state,
                  msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_SEARCH_NAME),
                  EXPLORE_TYPE_SEARCH);
            if (new_cbs)
               new_cbs->action_ok = explore_action_ok_find;
         }
         else
            explore_menu_entry(list, explore_state,
                  msg_hash_to_str(MENU_ENUM_LABEL_VALUE_EXPLORE_ADD_ADDITIONAL_FILTER),
                  EXPLORE_TYPE_ADDITIONALFILTER);
         explore_menu_add_spacer(list);
         explore_state->show_icons = EXPLORE_ICONS_CONTENT;
      }

      for (i = 1; i < depth; i++)
      {
         explore_string_t **entries = NULL;
         unsigned by_selected_type  = 0;
         unsigned by_category       = (stack_top[i].type 
               - EXPLORE_TYPE_FIRSTCATEGORY);

         if (by_category >= EXPLORE_CAT_COUNT)
            continue;

         by_selected_type           = stack_top[i + 1].type;
         entries                    = explore_state->by[by_category];
         cats     [levels]          = by_category;
         use_split[levels]          = explore_by_info[by_category].use_split;
         filter   [levels]          = 
            (by_selected_type == EXPLORE_TYPE_FILTERNULL 
             ? NULL 
             : entries[by_selected_type - EXPLORE_TYPE_FIRSTITEM]);
         levels++;
      }

      e                             = explore_state->entries;
      e_end                         = RBUF_END(explore_state->entries);

      for (; e != e_end; e++)
      {
         unsigned lvl;
         for (lvl = 0; lvl != levels; lvl++)
         {
            if (filter[lvl] == e->by[cats[lvl]])
               continue;
            if (use_split[lvl] && e->split)
            {
               explore_string_t** split = e->split;
               do
               {
                  if (*split == filter[lvl])
                     break;
               } while (*(++split));
               if (*split)
                  continue;
            }
            goto SKIP_ENTRY;
         }

         if (use_find && 
               !strcasestr(e->playlist_entry->label,
                  explore_state->find_string))
            goto SKIP_ENTRY;

         if (is_filtered_category)
         {
            explore_string_t* str = e->by[current_cat];
            if (!str)
            {
               filtered_category_have_unknown = true;
               continue;
            }
            if (ex_hashmap32_getnum(&map_filtered_category, str->idx + 1))
               continue;
            ex_hashmap32_setnum(&map_filtered_category, str->idx + 1, 1);
            explore_menu_entry(list, explore_state,
                  str->str,
                  EXPLORE_TYPE_FIRSTITEM + str->idx);
         }
#ifdef EXPLORE_SHOW_ORIGINAL_TITLE
         else if (e->original_title)
            explore_menu_entry(list,
                  explore_state, e->original_title,
                  EXPLORE_TYPE_FIRSTITEM + (e - explore_state->entries));
#endif
         else
            explore_menu_entry(list,
                  explore_state, e->playlist_entry->label,
                  EXPLORE_TYPE_FIRSTITEM + (e - explore_state->entries));

SKIP_ENTRY:;
      }

      if (is_filtered_category)
         qsort(list->list, list->size, sizeof(*list->list), explore_qsort_func_menulist);

      if (is_filtered_category && filtered_category_have_unknown)
      {
         explore_menu_add_spacer(list);
         explore_menu_entry(list, explore_state,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_UNKNOWN),
               EXPLORE_TYPE_FILTERNULL);
      }

      explore_append_title(explore_state,
            " (%u)", (unsigned) (list->size - (is_filtered_category ? 0 : 1)));

      ex_hashmap32_free(&map_filtered_category);
   }
   else
   {
      /* Content page of selected game */
      int pl_idx;
      const struct playlist_entry *pl_entry = 
         explore_state->entries[current_type - EXPLORE_TYPE_FIRSTITEM].playlist_entry;
      menu_handle_t                   *menu = menu_driver_get_ptr();

      strlcpy(explore_state->title,
            pl_entry->label, sizeof(explore_state->title));

      for (pl_idx = 0; pl_idx != RBUF_LEN(explore_state->playlists); pl_idx++)
      {
         menu_displaylist_info_t          info;
         const struct playlist_entry* pl_first = NULL;
         playlist_t                       *pl  = 
            explore_state->playlists[pl_idx];

         menu_displaylist_info_init(&info);

         playlist_get_index(pl, 0, &pl_first);

         if (  pl_entry <  pl_first || 
               pl_entry >= pl_first + playlist_size(pl))
            continue;

         /* Fake all the state so the content screen 
          * and information screen think we're viewing via playlist */
         playlist_set_cached_external(pl);
         menu->rpl_entry_selection_ptr = (pl_entry - pl_first);
         strlcpy(menu->deferred_path,
               pl_entry->path, sizeof(menu->deferred_path));
         info.list                     = list;
         menu_displaylist_ctl(DISPLAYLIST_HORIZONTAL_CONTENT_ACTIONS, &info);
         break;
      }
   }

   return list->size;
}

uintptr_t menu_explore_get_entry_icon(unsigned type)
{
   unsigned i;
   if (!explore_state || !explore_state->show_icons
         || type < EXPLORE_TYPE_FIRSTITEM)
      return 0;

   i = (type - EXPLORE_TYPE_FIRSTITEM);
   if (explore_state->show_icons == EXPLORE_ICONS_CONTENT)
   {
      explore_entry_t* e = &explore_state->entries[i];
      if (e < RBUF_END(explore_state->entries))
         return explore_state->icons[e->by[EXPLORE_BY_SYSTEM]->idx];
   }
   else if (explore_state->show_icons == EXPLORE_ICONS_SYSTEM_CATEGORY)
   {
      if (i < RBUF_LEN(explore_state->icons))
         return explore_state->icons[i];
   }
   return 0;
}

void menu_explore_context_init(void)
{
   if (!explore_state)
      return;

   explore_load_icons(explore_state);
}

void menu_explore_context_deinit(void)
{
   if (!explore_state)
      return;

   explore_unload_icons(explore_state);
}

void menu_explore_free(void)
{
   if (!explore_state)
      return;

   explore_free(explore_state);
   free(explore_state);
   explore_state = NULL;
}