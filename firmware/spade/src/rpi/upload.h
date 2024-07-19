#include "shared/sprig_engine/script.h"
#include "hardware/flash.h"
#include <stdlib.h>
static void core1_entry(void);

typedef enum {
    Location_FLASH
    // TODO: will add SD
    // TODO: swap games into SD if flash overfilled
} Game_Location;

typedef struct {
    char name[100]; // todo: we need to set frontend limits on game names
    Game_Location location;
    uint8_t slot;
    uint32_t size_b;
} Game;

Game current_game = (Game) {};
int slot = 0;

// rationale: half engine, half games?
// NOTE: this has to be a multiple of 4096 (FLASH_SECTOR_SIZE)
#define SLOT_SIZE FLASH_SECTOR_SIZE
#define MAX_SLOTS 150
#define FLASH_TARGET_START (800*1024)
#define FLASH_TARGET_OFFSET(slot_i) (FLASH_TARGET_START + slot_i * SLOT_SIZE)

#define GAME_SLOTS(bytes) ((bytes + ARR_LEN(engine_script) - 1) / SLOT_SIZE + 1)

#define METADATA_ENTRY_SIZE (256) // you can program up to one page at a time. (256 bytes).
#define METADATA_SIZE (32 * METADATA_ENTRY_SIZE) // first entry is for version // TODO: make sure this works when changing to 32
#define METADATA_MAX_ENTRIES (METADATA_SIZE / METADATA_ENTRY_SIZE - 1)
#define METADATA_CONTENTS(index) ((const Game *) (XIP_BASE + FLASH_TARGET_START - METADATA_SIZE + METADATA_ENTRY_SIZE + index * METADATA_ENTRY_SIZE))
#define METADATA_OFFSET ((uint32_t) (FLASH_TARGET_START - METADATA_SIZE + METADATA_ENTRY_SIZE))

// i think what happened here is only FLASH_TARGET_START was cast to a Game*.
// METADATA_SIZE was a signed int. this coerced METADATA_OFFSET into a signed int,
// which was then passed into a function that accepted a uint32_t.
// this messed up the pointer and caused it to overwrite application code in the flash (XIP)
// #define METADATA_OFFSET ((const Game*) FLASH_TARGET_START - METADATA_SIZE + METADATA_ENTRY_SIZE)


#define METADATA_START METADATA_CONTENTS(-1)

#define FLASH_VERSION ((const char *) (XIP_BASE + FLASH_TARGET_START - METADATA_SIZE))

#define FLASH_TARGET_CONTENTS(slot_i) ((const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET(slot_i)))

#define ZEROS(bytes) ((char[bytes]){})

static const int memory_is_ones(const void *memory, size_t count) {
    // this is taken from gcc libiberty
    register const unsigned char *s = (const unsigned char*)memory;

    while (count-- > 0)
    {
        if (*s++ != 0xFF)
            return 0;
    }
    return 1;
}

// sprig magic is still here because it's useful for debugging (binvis.io) + easier to not get rid of it lol
uint16_t SPRIG_MAGIC[6] = { 1337, 42, 69, 420, 420, 1337 };

static const char *save_read() {
    if (memcmp(&SPRIG_MAGIC, FLASH_TARGET_CONTENTS(slot), sizeof(SPRIG_MAGIC)) != 0) {
    puts("no magic :(");
    return NULL;
  }

  // add a page to get what's after the magic
    const char *save = FLASH_TARGET_CONTENTS(slot) + FLASH_PAGE_SIZE;
  return save;
}

static void erase_user_portion_of_flash_this_is_dangerous() {
    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(
            (uint32_t) FLASH_TARGET_START - METADATA_SIZE - METADATA_ENTRY_SIZE,
            SLOT_SIZE * MAX_SLOTS + METADATA_SIZE - METADATA_ENTRY_SIZE); // TODO: check that this is valid
    restore_interrupts(interrupts);
}

// save versions != spade versions
// increment in the value returned means different scheme for saving games
static int get_save_version(const char* version) {
    if (memory_is_ones(version, METADATA_ENTRY_SIZE)) return 1;
    if (strcmp(version, "1.1.0") == 0) return 2;

    // something weird happened!?
    return -1;
}

// must be run with core 1 disabled - irq risk
static void flash_write_save_version(const char* version) {
    void *metadata_first_sector = malloc(FLASH_SECTOR_SIZE);
    memcpy(metadata_first_sector, METADATA_START, FLASH_SECTOR_SIZE);
    strcpy(metadata_first_sector, version);

    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(METADATA_OFFSET - METADATA_ENTRY_SIZE, FLASH_SECTOR_SIZE);
    flash_range_program(METADATA_OFFSET - METADATA_ENTRY_SIZE, metadata_first_sector, FLASH_SECTOR_SIZE);
    restore_interrupts(interrupts);

    free(metadata_first_sector);
}

// must be run with core 1 disabled - irq risk
static int update_save_version() { // TODO: handle totally empty flash
    int version = get_save_version(FLASH_VERSION);
    if (version == get_save_version(SPADE_VERSION)) return 0;

    switch (version) { // recursive if need to update multiple versions.
        case 1: // 1.0.0 or less
        {
            if (memcmp(&SPRIG_MAGIC, FLASH_TARGET_CONTENTS(0), sizeof(SPRIG_MAGIC)) == 0) {
                // add flash metadata for first game
                Game game = {
                        .slot = 0,
                        .size_b = strlen(FLASH_TARGET_CONTENTS(0)),
                        .name = "Legacy Game",
                        .location = Location_FLASH
                };

                void *metadata_first_sector = malloc(FLASH_SECTOR_SIZE);
                memcpy(metadata_first_sector, METADATA_START, FLASH_SECTOR_SIZE);
                memcpy(metadata_first_sector + METADATA_ENTRY_SIZE, &game, sizeof(Game));

                uint32_t interrupts = save_and_disable_interrupts();
                flash_range_erase(METADATA_OFFSET - METADATA_ENTRY_SIZE, FLASH_SECTOR_SIZE);
                flash_range_program(METADATA_OFFSET - METADATA_ENTRY_SIZE, metadata_first_sector,
                                    FLASH_SECTOR_SIZE);
                restore_interrupts(interrupts);

                free(metadata_first_sector);
            }

            flash_write_save_version("1.1.0");
            return update_save_version();
        }
        case 2:
        default:
            return 1;
    }

}

// returns len of games. games should be pointer to array of games (Game**)
static int get_games(Game** games, int games_len) {
    int games_i = -1;

    for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {

        volatile int is_zeros = memcmp(METADATA_CONTENTS(i), ZEROS(sizeof(Game)), sizeof(Game));
        volatile int is_ones = memory_is_ones(METADATA_CONTENTS(i), METADATA_ENTRY_SIZE);
        if (is_ones) continue;

        if (games_len <= ++games_i) {
            games_len *= 2;
            *games = realloc(*games, games_len * sizeof(Game));
        }

        (*games)[games_i] = *METADATA_CONTENTS(i);
    }

    return games_i + 1;
}

static int get_available_game_slots() {
    int available_metadata_slots = 0;
    int available_flash_slots = MAX_SLOTS;
    for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {
        volatile int is_ones = memory_is_ones(METADATA_CONTENTS(i), METADATA_ENTRY_SIZE);
        if (is_ones)
            available_metadata_slots++;
        else
            available_flash_slots -= (int) GAME_SLOTS(METADATA_CONTENTS(i)->size_b);
    }

    // minimum of both
    return available_flash_slots < available_metadata_slots ? available_flash_slots : available_metadata_slots;
}


static int get_first_open_flash_slot_at_end() {
    int lowest_open_slot = -1;
    for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {
        if (!memory_is_ones(METADATA_CONTENTS(i), METADATA_ENTRY_SIZE)) {
            int slot_after_entry = METADATA_CONTENTS(i)->slot + GAME_SLOTS(METADATA_CONTENTS(i)->size_b);

            if (slot_after_entry > lowest_open_slot) {
                lowest_open_slot = slot_after_entry;
            }
        }
    }
    return lowest_open_slot + 1;
}

static int get_available_flash_slots_at_end() {
    return MAX_SLOTS - get_first_open_flash_slot_at_end();
}

static int get_first_open_metadata_slot() {
    for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {
        if (memory_is_ones(METADATA_CONTENTS(i), METADATA_ENTRY_SIZE))
            return i;
    }

    return -1;
}

static void set_game(Game aGame) {
    // this is fine for now.
    // in future, include logic to swap games in SD to flash
    current_game = aGame;
    slot = aGame.slot;
}

static int get_game_index_by_name(char* name) {
    for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {
        if (!memory_is_ones(METADATA_CONTENTS(i), METADATA_ENTRY_SIZE)
        && strcmp(METADATA_CONTENTS(i)->name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static void delete_game(Game aGame) {
    for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {
        if (memcmp(METADATA_CONTENTS(i), &aGame, sizeof(Game)) == 0) { // this is the metadata slot
                char ones[METADATA_ENTRY_SIZE];
                for (int j = 0; j < METADATA_ENTRY_SIZE; j++) {
                    ones[j] = 0xFF;
                }

            void *new_metadata = malloc(METADATA_SIZE);
            memcpy(new_metadata, METADATA_START, METADATA_SIZE);
            memcpy(new_metadata + METADATA_ENTRY_SIZE * (i + 1), &ones, METADATA_ENTRY_SIZE);

            multicore_reset_core1();
            uint32_t interrupts = save_and_disable_interrupts();

            flash_range_erase(METADATA_OFFSET - METADATA_ENTRY_SIZE, METADATA_SIZE);
            flash_range_program(METADATA_OFFSET - METADATA_ENTRY_SIZE, new_metadata, METADATA_SIZE);

            free(new_metadata);
            restore_interrupts(interrupts);
            multicore_launch_core1(core1_entry);
        }
    }
}

// TODO: proofread/test
// TODO: this does not work!
void consolidate_flash_games() {
    multicore_reset_core1();

    void *new_metadata = malloc(METADATA_SIZE);
    memcpy(new_metadata, METADATA_START, METADATA_SIZE);

    int last_contiguous_game = 0;

    int prev_write = -1;

    for (;;) {
        // infinite loop terminated by hitting end:

        int write = 0;

        int read = MAX_SLOTS;
        int read_metadata_i = 0;

        for (int i = 0; i < METADATA_MAX_ENTRIES; i++) {

            int empty = 1;

            for (int j = 0; j < METADATA_MAX_ENTRIES; j++) {
                if (METADATA_CONTENTS(j)->slot ==
                    METADATA_CONTENTS(i)->slot + GAME_SLOTS(METADATA_CONTENTS(i)->size_b)) {
                    empty = 0;
                }
            }

            if (!empty) continue;

            write = METADATA_CONTENTS(i)->slot + GAME_SLOTS(METADATA_CONTENTS(i)->size_b);

            if (write == prev_write) {

                uint32_t interrupts = save_and_disable_interrupts();

                flash_range_erase(METADATA_OFFSET - METADATA_ENTRY_SIZE, METADATA_SIZE);
                flash_range_program(METADATA_OFFSET - METADATA_ENTRY_SIZE, new_metadata, METADATA_SIZE);

                restore_interrupts(interrupts);

                free(new_metadata);
                multicore_launch_core1(core1_entry);
                return;
            }

            for (int j = 0; j < METADATA_MAX_ENTRIES; j++) {
                if (METADATA_CONTENTS(j)->slot > write && METADATA_CONTENTS(j)->slot < read) {
                    read = METADATA_CONTENTS(j)->slot;
                    read_metadata_i = j;
                }
            }

            prev_write = write;

            break;
        }

        for (int i = 0; i < GAME_SLOTS(METADATA_CONTENTS(read_metadata_i)->size_b); i++) {
            uint32_t interrupts = save_and_disable_interrupts();

            flash_range_erase(FLASH_TARGET_OFFSET(read + i), SLOT_SIZE);
            flash_range_program(FLASH_TARGET_OFFSET(read + FLASH_SECTOR_SIZE * i), FLASH_TARGET_CONTENTS(write + i),
                                SLOT_SIZE);

            restore_interrupts(interrupts);
        }

        ((Game*) (new_metadata + METADATA_ENTRY_SIZE*read))->slot = write;

    }
}

typedef enum {
    UplProg_Init,
  UplProg_Header,
  UplProg_Body,
} UplProg;
static struct {
  UplProg prog;
  uint32_t len, len_i;
  char buf[256];
  int page;
  char name[100];
  uint8_t name_i;
} upl_state = {0};

static void upl_flush_buf(void) {
    puts("wtf?? 6");

    uint32_t interrupts = save_and_disable_interrupts();
  flash_range_program(FLASH_TARGET_OFFSET(slot) + (upl_state.page++) * 256,
                      (void *)upl_state.buf,
                      256);
  restore_interrupts(interrupts);
  memset(upl_state.buf, 0, sizeof(upl_state.buf));
  printf("wrote page (%d/%lu)\n",
         upl_state.page,
         (upl_state.len/(FLASH_PAGE_SIZE + 1)));
}

static int upl_stdin_read(void) {
    memset(&upl_state, 0, sizeof(upl_state));

    int timeout = 1000; // 1ms; we're already in upload mode

  for (;;) {
    int c = getchar_timeout_us(timeout);
    if (c == PICO_ERROR_TIMEOUT) return 0;

    switch (upl_state.prog) {
        case UplProg_Init: {
            // irqs on other core?
            multicore_reset_core1();

            upl_state.prog = UplProg_Header;
        } // falls through
      case UplProg_Header: {
          puts("wahoo header");

          if (upl_state.name_i < sizeof(upl_state.name) / sizeof(char)) // read game
          {
              ((char *) (&upl_state.name))[upl_state.name_i++] = c;
              puts(upl_state.name);
          }
          else {
              ((char *) (&upl_state.len))[upl_state.len_i++] = c;
              if (upl_state.len_i >= sizeof(uint32_t)) {
                  printf("ok reading %lu chars\n", upl_state.len);
                  upl_state.prog = UplProg_Body;
                  upl_state.len_i = 0;
                  upl_state.page = 1; // skip first, that's for magic

                  {
                      Game game;
                      strcpy(game.name, upl_state.name);
                      game.size_b = upl_state.len;
                      game.location = Location_FLASH;


                      // not enough slots
                      if (get_available_game_slots() < GAME_SLOTS(upl_state.len)) {
                          puts("no available game slots!");
                          printf("we need %lu slots but we only have %d",GAME_SLOTS(upl_state.len), get_available_game_slots());
                          return 0; // ERROR!
                      } else if (get_available_flash_slots_at_end() < GAME_SLOTS(upl_state.len)) {
                          puts("consolidating!");
                          consolidate_flash_games();
                          game.slot = get_first_open_flash_slot_at_end();
                      } else {
                          game.slot = get_first_open_flash_slot_at_end();
                      }

                      slot = game.slot;

                      int metadata_i;
                      int search_result = get_game_index_by_name(game.name);

                      if (search_result != -1) {
                          metadata_i = search_result;
                      } else {
                          metadata_i = get_first_open_metadata_slot();
                      }
                      void *new_metadata = malloc(METADATA_SIZE);
                      memcpy(new_metadata, METADATA_START, METADATA_SIZE);
                      memcpy(new_metadata + METADATA_ENTRY_SIZE * (metadata_i + 1), &game, sizeof(Game));

                      uint32_t interrupts = save_and_disable_interrupts();
                      flash_range_erase(METADATA_OFFSET - METADATA_ENTRY_SIZE, METADATA_SIZE);
                      flash_range_program(METADATA_OFFSET - METADATA_ENTRY_SIZE, new_metadata, METADATA_SIZE);
                      restore_interrupts(interrupts);

                      printf("metadata_i: %d,\n"
                             "slot: %d\n"
                             "size_b: %lu", metadata_i, slot, game.size_b);

                      free(new_metadata);
                  }

                  uint32_t char_len = upl_state.len +
                                 sizeof(engine_script); // sizeof script includes the null term, we still need to remove from script
                  upl_state.len = char_len;
                  // one to round up, one for magic
                  uint32_t page_len = (char_len / FLASH_PAGE_SIZE + 2) * FLASH_PAGE_SIZE;
                  uint32_t sector_len = (page_len / FLASH_SECTOR_SIZE + 1) * FLASH_SECTOR_SIZE;

                  uint32_t interrupts = save_and_disable_interrupts();
                  flash_range_erase(FLASH_TARGET_OFFSET(slot), sector_len);
                  restore_interrupts(interrupts);

                  for (int i = 0; i < sizeof(engine_script) - 1; i++) {
                      upl_state.buf[upl_state.len_i++ % FLASH_PAGE_SIZE] = engine_script[i];
                      if (upl_state.len_i % FLASH_PAGE_SIZE == 0) {
                          puts("flushin buf (wit da code!)");
                          upl_flush_buf();
                      }
                  }

                  puts("cleared flash");
              }
          }
      } break;
      case UplProg_Body: {

          // printf("upl char (%d/%d)\n", upl_state.len_i, upl_state.len);
        upl_state.buf[upl_state.len_i++ % FLASH_PAGE_SIZE] = c;

        if (upl_state.len_i % FLASH_PAGE_SIZE == 0) {
          puts("flushin buf");
          upl_flush_buf();
        }

          if (upl_state.len_i == upl_state.len - 1) {
          upl_flush_buf();

          uint32_t interrupts = save_and_disable_interrupts();
          flash_range_program(FLASH_TARGET_OFFSET(slot), (void *)SPRIG_MAGIC, FLASH_PAGE_SIZE);
          restore_interrupts(interrupts);

          // printf("read in %d chars\n", upl_state.len);
          puts("ALL_GOOD");
          memset(&upl_state, 0, sizeof(upl_state));

          multicore_launch_core1(core1_entry);

          return 1;
        }
      } break;
    }
  }
  puts("end of upl_stdin_read");
}
