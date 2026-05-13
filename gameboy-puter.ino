#define ENABLE_SOUND 0
#define ENABLE_LCD 1

#ifndef GBPUTER_FAST_DMG
#define GBPUTER_FAST_DMG 0
#endif

#if GBPUTER_FAST_DMG
#ifndef WALNUT_FULL_GBC_SUPPORT
#define WALNUT_FULL_GBC_SUPPORT 0
#endif
#ifndef WALNUT_GB_12_COLOUR
#define WALNUT_GB_12_COLOUR 0
#endif
#ifndef WALNUT_GB_HIGH_LCD_ACCURACY
#define WALNUT_GB_HIGH_LCD_ACCURACY 0
#endif
#endif

#define MAX_ROMS 256
#define PATH_SIZE 128
#define NAME_SIZE 72
#define ROM_CACHE_PAGE_SIZE 4096
#define ROM_CACHE_PAGES 8
#define ROM_HEAP_RESERVE_BYTES (72 * 1024)
#define PERF_REPORT_INTERVAL_MS 3000
#define FRAME_SKIP_SAMPLE_MS 3000

#include "M5Cardputer.h"
#include "SD.h"
#include "esp_heap_caps.h"
// Obtain the latest version of walnut_cgb.h here:
// https://github.com/Mr-PauI/Walnut-CGB
#include "./walnut-cgb/walnut_cgb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define DEST_W 240
#define DEST_H 135

SPIClass SPI2;

struct RomEntry {
  char path[PATH_SIZE];
  char name[NAME_SIZE];
};

struct priv_t {
  File rom_file;
  size_t rom_size;
  uint8_t *rom_data;
  bool rom_in_ram;
  uint8_t *rom_cache;
  uint32_t rom_cache_pages[ROM_CACHE_PAGES];
  uint32_t rom_cache_ages[ROM_CACHE_PAGES];
  bool rom_cache_valid[ROM_CACHE_PAGES];
  uint32_t rom_cache_clock;
  int8_t last_cache_slot;
  uint32_t rom_cache_hits;
  uint32_t rom_cache_misses;
  uint8_t *cart_ram;
  size_t cart_ram_size;
  bool cart_ram_dirty;
  char save_path[PATH_SIZE];
  char error_text[NAME_SIZE];

  uint16_t fb[LCD_HEIGHT][LCD_WIDTH];
};

struct PerfStats {
  uint32_t frames;
  uint64_t frame_work_us;
  uint64_t emu_us;
  uint64_t draw_us;
  uint32_t report_start_ms;
  uint32_t sample_start_ms;
  bool frame_skip_checked;
};

static RomEntry *roms = nullptr;
static int rom_count = 0;
static bool rom_list_truncated = false;
static bool sd_bus_started = false;
static bool sd_ready = false;

static uint16_t rgb888_to_rgb565(uint32_t rgb) {
  return (uint16_t)(((rgb >> 8) & 0xF800) |
                    ((rgb >> 5) & 0x07E0) |
                    ((rgb >> 3) & 0x001F));
}

#if WALNUT_GB_12_COLOUR
uint32_t gboriginal_palette[] = {
    0x7B8210, 0x5A7942, 0x39594A, 0x294139,
    0x7B8210, 0x5A7942, 0x39594A, 0x294139,
    0x7B8210, 0x5A7942, 0x39594A, 0x294139};
uint16_t CURRENT_PALETTE_RGB565[12];
void update_palette() {
  for (int i = 0; i < 12; i++) {
    CURRENT_PALETTE_RGB565[i] = rgb888_to_rgb565(gboriginal_palette[i]);
  }
}
#else
uint32_t gboriginal_palette[] = {0x7B8210, 0x5A7942, 0x39594A, 0x294139};
uint16_t CURRENT_PALETTE_RGB565[4];
void update_palette() {
  for (int i = 0; i < 4; i++) {
    CURRENT_PALETTE_RGB565[i] = rgb888_to_rgb565(gboriginal_palette[i]);
  }
}
#endif

void set_font_size(int size) {
  int textsize = M5Cardputer.Display.height() / size;
  if (textsize == 0) {
    textsize = 1;
  }
  M5Cardputer.Display.setTextSize(textsize);
}

void show_message(const char *title, const char *line1 = nullptr,
                  const char *line2 = nullptr) {
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setCursor(0, 0);
  set_font_size(72);
  M5Cardputer.Display.println(title);
  set_font_size(110);
  if (line1 != nullptr) {
    M5Cardputer.Display.println(line1);
  }
  if (line2 != nullptr) {
    M5Cardputer.Display.println(line2);
  }
}

int compare_ignore_case(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    int ca = tolower((unsigned char)*a);
    int cb = tolower((unsigned char)*b);
    if (ca != cb) {
      return ca - cb;
    }
    a++;
    b++;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

bool is_rom_file(const char *name) {
  const char *dot = strrchr(name, '.');
  if (dot == nullptr) {
    return false;
  }

  char ext[5] = {0};
  strncpy(ext, dot + 1, sizeof(ext) - 1);
  for (size_t i = 0; ext[i] != '\0'; i++) {
    ext[i] = (char)tolower((unsigned char)ext[i]);
  }

  if (strcmp(ext, "gb") == 0) {
    return true;
  }

#if GBPUTER_FAST_DMG
  return false;
#else
  return strcmp(ext, "gbc") == 0;
#endif
}

void sort_roms() {
  if (roms == nullptr) {
    return;
  }

  for (int i = 1; i < rom_count; i++) {
    RomEntry entry = roms[i];
    int j = i - 1;
    while (j >= 0 && compare_ignore_case(roms[j].name, entry.name) > 0) {
      roms[j + 1] = roms[j];
      j--;
    }
    roms[j + 1] = entry;
  }
}

void add_rom_entry(const char *dir, const char *file_name) {
  if (roms == nullptr) {
    return;
  }

  if (rom_count >= MAX_ROMS) {
    rom_list_truncated = true;
    return;
  }

  const char *name = base_name(file_name);
  RomEntry &entry = roms[rom_count];

  if (strcmp(dir, "/") == 0) {
    snprintf(entry.path, sizeof(entry.path), "/%s", name);
    snprintf(entry.name, sizeof(entry.name), "%s", name);
  } else {
    snprintf(entry.path, sizeof(entry.path), "%s/%s", dir, name);
    snprintf(entry.name, sizeof(entry.name), "%s", name);
  }

  rom_count++;
}

void scan_directory_for_roms(const char *dir) {
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    return;
  }

  while (rom_count < MAX_ROMS) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }

    if (!entry.isDirectory()) {
      const char *name = base_name(entry.name());
      if (is_rom_file(name)) {
        add_rom_entry(dir, name);
      }
    }
    entry.close();
  }

  File overflow = root.openNextFile();
  if (overflow) {
    rom_list_truncated = true;
    overflow.close();
  }

  root.close();
}

bool ensure_rom_list() {
  if (roms != nullptr) {
    return true;
  }

  roms = (RomEntry *)calloc(MAX_ROMS, sizeof(RomEntry));
  return roms != nullptr;
}

void release_rom_list() {
  if (roms != nullptr) {
    free(roms);
    roms = nullptr;
  }
  rom_count = 0;
}

bool scan_roms() {
  if (!ensure_rom_list()) {
    return false;
  }

  rom_count = 0;
  rom_list_truncated = false;
  scan_directory_for_roms("/roms");
  scan_directory_for_roms("/");
  sort_roms();
  return true;
}

void wait_for_key_release() {
  do {
    M5Cardputer.update();
    delay(20);
  } while (M5Cardputer.Keyboard.isPressed());
}

void wait_for_enter() {
  wait_for_key_release();
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed() &&
        M5Cardputer.Keyboard.keysState().enter) {
      wait_for_key_release();
      return;
    }
    delay(20);
  }
}

bool mount_sd_card(bool show_status = false) {
  if (show_status) {
    show_message("SD card", "Checking microSD...");
  }

  if (!sd_bus_started) {
    SPI2.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk),
               M5.getPin(m5::pin_name_t::sd_spi_miso),
               M5.getPin(m5::pin_name_t::sd_spi_mosi),
               M5.getPin(m5::pin_name_t::sd_spi_ss));
    sd_bus_started = true;
  }

  sd_ready = SD.begin(M5.getPin(m5::pin_name_t::sd_spi_ss), SPI2);
  return sd_ready;
}

bool sd_card_available() {
  if (!sd_ready) {
    return false;
  }

  File root = SD.open("/");
  if (!root) {
    sd_ready = false;
    return false;
  }

  root.close();
  return true;
}

void render_picker(int selected) {
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.setTextColor(WHITE, BLACK);

  set_font_size(110);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.printf("ROMs %d/%d\n", selected + 1, rom_count);
  if (rom_list_truncated) {
    M5Cardputer.Display.println("List limited");
  } else {
    M5Cardputer.Display.println("/roms + root");
  }

  int center_y = 61;
  int line_h = 22;
  for (int row = -2; row <= 2; row++) {
    int index = selected + row;
    if (index < 0 || index >= rom_count) {
      continue;
    }

    int y = center_y + row * line_h;
    set_font_size(row == 0 ? 100 : 128);
    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print(row == 0 ? "> " : "  ");
    M5Cardputer.Display.print(roms[index].name);
  }

  set_font_size(135);
  M5Cardputer.Display.setCursor(0, 122);
  M5Cardputer.Display.print(";/. move  Enter play");
}

bool pick_rom(char *selected_path, size_t selected_path_len) {
  while (true) {
    if (!sd_card_available() && !mount_sd_card(true)) {
      show_message("Gameboy-Puter", "No SD card", "Enter: retry");
      wait_for_enter();
      continue;
    }

    if (!scan_roms()) {
      show_message("Launcher RAM failed", "Could not list ROMs", "Enter: retry");
      wait_for_enter();
      return false;
    }

    if (rom_count > 0) {
      break;
    }

    show_message("No ROMs found", "Put .gb/.gbc in /roms", "Enter: rescan");
    wait_for_enter();
  }

  int selected = 0;
  int last_rendered = -1;

  while (true) {
    if (selected != last_rendered) {
      render_picker(selected);
      last_rendered = selected;
    }

    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

      for (char key : status.word) {
        switch (key) {
          case ';':
          case 'e':
          case 'w':
            selected--;
            if (selected < 0) {
              selected = rom_count - 1;
            }
            delay(160);
            break;
          case '.':
          case 's':
            selected++;
            if (selected >= rom_count) {
              selected = 0;
            }
            delay(160);
            break;
          default:
            break;
        }
      }

      if (status.enter) {
        snprintf(selected_path, selected_path_len, "%s", roms[selected].path);
        release_rom_list();
        wait_for_key_release();
        return true;
      }
    }

    delay(20);
  }
}

uint8_t *rom_cache_page_for_addr(priv_t *p, const uint_fast32_t addr,
                                 uint32_t *offset) {
  if (addr >= p->rom_size) {
    return nullptr;
  }

  const uint32_t page = addr / ROM_CACHE_PAGE_SIZE;
  *offset = addr % ROM_CACHE_PAGE_SIZE;
  p->rom_cache_clock++;

  if (p->last_cache_slot >= 0 &&
      p->rom_cache_valid[p->last_cache_slot] &&
      p->rom_cache_pages[p->last_cache_slot] == page) {
    p->rom_cache_hits++;
    p->rom_cache_ages[p->last_cache_slot] = p->rom_cache_clock;
    return p->rom_cache + p->last_cache_slot * ROM_CACHE_PAGE_SIZE;
  }

  for (int i = 0; i < ROM_CACHE_PAGES; i++) {
    if (p->rom_cache_valid[i] && p->rom_cache_pages[i] == page) {
      p->rom_cache_hits++;
      p->rom_cache_ages[i] = p->rom_cache_clock;
      p->last_cache_slot = i;
      return p->rom_cache + i * ROM_CACHE_PAGE_SIZE;
    }
  }

  p->rom_cache_misses++;
  int victim = 0;
  for (int i = 0; i < ROM_CACHE_PAGES; i++) {
    if (!p->rom_cache_valid[i]) {
      victim = i;
      break;
    }
    if (p->rom_cache_ages[i] < p->rom_cache_ages[victim]) {
      victim = i;
    }
  }

  uint8_t *page_data = p->rom_cache + victim * ROM_CACHE_PAGE_SIZE;
  const uint32_t file_offset = page * ROM_CACHE_PAGE_SIZE;
  memset(page_data, 0xFF, ROM_CACHE_PAGE_SIZE);

  if (!p->rom_file.seek(file_offset)) {
    return nullptr;
  }

  size_t remaining = p->rom_size - file_offset;
  size_t to_read = remaining < ROM_CACHE_PAGE_SIZE ? remaining : ROM_CACHE_PAGE_SIZE;
  size_t read = p->rom_file.read(page_data, to_read);
  if (read == 0) {
    return nullptr;
  }

  p->rom_cache_valid[victim] = true;
  p->rom_cache_pages[victim] = page;
  p->rom_cache_ages[victim] = p->rom_cache_clock;
  p->last_cache_slot = victim;
  return page_data;
}

uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
  struct priv_t *p = (struct priv_t *)gb->direct.priv;
  if (addr >= p->rom_size) {
    return 0xFF;
  }

  if (p->rom_in_ram) {
    return p->rom_data[addr];
  }

  uint32_t offset = 0;
  uint8_t *page_data = rom_cache_page_for_addr(p, addr, &offset);
  if (page_data == nullptr) {
    return 0xFF;
  }
  return page_data[offset];
}

uint16_t gb_rom_read_16bit(struct gb_s *gb, const uint_fast32_t addr) {
  struct priv_t *p = (struct priv_t *)gb->direct.priv;
  if (addr + 1 >= p->rom_size) {
    return (uint16_t)gb_rom_read(gb, addr) |
           ((uint16_t)gb_rom_read(gb, addr + 1) << 8);
  }

  if (p->rom_in_ram) {
    return (uint16_t)p->rom_data[addr] | ((uint16_t)p->rom_data[addr + 1] << 8);
  }

  if ((addr / ROM_CACHE_PAGE_SIZE) == ((addr + 1) / ROM_CACHE_PAGE_SIZE)) {
    uint32_t offset = 0;
    uint8_t *page_data = rom_cache_page_for_addr(p, addr, &offset);
    if (page_data != nullptr) {
      return (uint16_t)page_data[offset] | ((uint16_t)page_data[offset + 1] << 8);
    }
  }

  const uint8_t lo = gb_rom_read(gb, addr);
  const uint8_t hi = gb_rom_read(gb, addr + 1);
  return (uint16_t)lo | ((uint16_t)hi << 8);
}

uint32_t gb_rom_read_32bit(struct gb_s *gb, const uint_fast32_t addr) {
  struct priv_t *p = (struct priv_t *)gb->direct.priv;
  if (addr + 3 < p->rom_size) {
    if (p->rom_in_ram) {
      return (uint32_t)p->rom_data[addr] |
             ((uint32_t)p->rom_data[addr + 1] << 8) |
             ((uint32_t)p->rom_data[addr + 2] << 16) |
             ((uint32_t)p->rom_data[addr + 3] << 24);
    }

    if ((addr / ROM_CACHE_PAGE_SIZE) == ((addr + 3) / ROM_CACHE_PAGE_SIZE)) {
      uint32_t offset = 0;
      uint8_t *page_data = rom_cache_page_for_addr(p, addr, &offset);
      if (page_data != nullptr) {
        return (uint32_t)page_data[offset] |
               ((uint32_t)page_data[offset + 1] << 8) |
               ((uint32_t)page_data[offset + 2] << 16) |
               ((uint32_t)page_data[offset + 3] << 24);
      }
    }
  }

  return (uint32_t)gb_rom_read(gb, addr) |
         ((uint32_t)gb_rom_read(gb, addr + 1) << 8) |
         ((uint32_t)gb_rom_read(gb, addr + 2) << 16) |
         ((uint32_t)gb_rom_read(gb, addr + 3) << 24);
}

uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  const struct priv_t *const p = (const struct priv_t *)gb->direct.priv;
  if (p->cart_ram == nullptr || addr >= p->cart_ram_size) {
    return 0xFF;
  }
  return p->cart_ram[addr];
}

void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                       const uint8_t val) {
  struct priv_t *p = (struct priv_t *)gb->direct.priv;
  if (p->cart_ram == nullptr || addr >= p->cart_ram_size) {
    return;
  }

  if (p->cart_ram[addr] != val) {
    p->cart_ram[addr] = val;
    p->cart_ram_dirty = true;
  }
}

const char *gb_init_error_name(enum gb_init_error_e err) {
  switch (err) {
    case GB_INIT_NO_ERROR:
      return "No error";
    case GB_INIT_CARTRIDGE_UNSUPPORTED:
      return "Unsupported cartridge";
    case GB_INIT_INVALID_CHECKSUM:
      return "Invalid checksum";
    default:
      return "Init failed";
  }
}

const char *gb_runtime_error_name(enum gb_error_e err) {
  switch (err) {
    case GB_INVALID_OPCODE:
      return "Invalid opcode";
    case GB_INVALID_READ:
      return "Invalid read";
    case GB_INVALID_WRITE:
      return "Invalid write";
    default:
      return "Emulator error";
  }
}

void gb_error(struct gb_s *gb, const enum gb_error_e gb_err,
              const uint16_t val) {
  struct priv_t *priv = (struct priv_t *)gb->direct.priv;
  snprintf(priv->error_text, sizeof(priv->error_text), "%s %04X",
           gb_runtime_error_name(gb_err), val);
}

bool should_load_rom_to_ram(size_t rom_size) {
  const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (free_heap <= ROM_HEAP_RESERVE_BYTES) {
    return false;
  }
  return rom_size < (free_heap - ROM_HEAP_RESERVE_BYTES) &&
         rom_size <= largest_block;
}

bool open_rom_cache(priv_t *priv, const char *file_name, char *error,
                    size_t error_len) {
  priv->rom_file = SD.open(file_name, FILE_READ);
  if (!priv->rom_file) {
    snprintf(error, error_len, "Could not open ROM");
    return false;
  }

  priv->rom_size = priv->rom_file.size();
  if (priv->rom_size < 0x150) {
    priv->rom_file.close();
    snprintf(error, error_len, "File is too small");
    return false;
  }

  if (should_load_rom_to_ram(priv->rom_size)) {
    priv->rom_data = (uint8_t *)malloc(priv->rom_size);
    if (priv->rom_data != nullptr) {
      size_t read = priv->rom_file.read(priv->rom_data, priv->rom_size);
      priv->rom_file.close();
      if (read != priv->rom_size) {
        free(priv->rom_data);
        priv->rom_data = nullptr;
        snprintf(error, error_len, "Could not read ROM");
        return false;
      }
      priv->rom_in_ram = true;
      Serial.printf("[rom] %s size=%u backing=ram free_heap=%u\n",
                    base_name(file_name), (unsigned int)priv->rom_size,
                    (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
      return true;
    }
  }

  priv->rom_cache = (uint8_t *)malloc(ROM_CACHE_PAGE_SIZE * ROM_CACHE_PAGES);
  if (priv->rom_cache == nullptr) {
    priv->rom_file.close();
    snprintf(error, error_len, "No RAM for ROM cache");
    return false;
  }

  memset(priv->rom_cache, 0xFF, ROM_CACHE_PAGE_SIZE * ROM_CACHE_PAGES);
  for (int i = 0; i < ROM_CACHE_PAGES; i++) {
    priv->rom_cache_pages[i] = 0;
    priv->rom_cache_ages[i] = 0;
    priv->rom_cache_valid[i] = false;
  }
  priv->rom_cache_clock = 0;
  priv->last_cache_slot = -1;
  Serial.printf("[rom] %s size=%u backing=sd cache=%u free_heap=%u\n",
                base_name(file_name), (unsigned int)priv->rom_size,
                (unsigned int)(ROM_CACHE_PAGE_SIZE * ROM_CACHE_PAGES),
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT));

  return true;
}

void replace_extension(char *path, size_t path_len, const char *extension) {
  char *dot = strrchr(path, '.');
  if (dot == nullptr) {
    size_t used = strlen(path);
    if (used < path_len) {
      snprintf(path + used, path_len - used, "%s", extension);
    }
  } else {
    snprintf(dot, path_len - (dot - path), "%s", extension);
  }
}

void make_save_path(const char *rom_path, char *save_path, size_t save_path_len) {
  snprintf(save_path, save_path_len, "%s", rom_path);
  replace_extension(save_path, save_path_len, ".sav");
}

bool load_save(priv_t *priv) {
  if (priv->cart_ram_size == 0 || priv->cart_ram == nullptr) {
    return true;
  }

  memset(priv->cart_ram, 0, priv->cart_ram_size);
  File save = SD.open(priv->save_path, FILE_READ);
  if (!save) {
    priv->cart_ram_dirty = false;
    return true;
  }

  size_t bytes_read = save.readBytes((char *)priv->cart_ram, priv->cart_ram_size);
  save.close();
  if (bytes_read < priv->cart_ram_size) {
    memset(priv->cart_ram + bytes_read, 0, priv->cart_ram_size - bytes_read);
  }

  priv->cart_ram_dirty = false;
  return true;
}

bool flush_save(priv_t *priv) {
  if (priv->cart_ram_size == 0 || priv->cart_ram == nullptr ||
      !priv->cart_ram_dirty) {
    return true;
  }

  char temp_path[PATH_SIZE] = {0};
  snprintf(temp_path, sizeof(temp_path), "%s", priv->save_path);
  replace_extension(temp_path, sizeof(temp_path), ".tmp");

  SD.remove(temp_path);
  File save = SD.open(temp_path, FILE_WRITE);
  if (!save) {
    return false;
  }

  size_t bytes_written = save.write(priv->cart_ram, priv->cart_ram_size);
  save.close();
  if (bytes_written != priv->cart_ram_size) {
    SD.remove(temp_path);
    return false;
  }

  SD.remove(priv->save_path);
  if (!SD.rename(temp_path, priv->save_path)) {
    SD.remove(temp_path);
    return false;
  }

  priv->cart_ram_dirty = false;
  return true;
}

void free_game(priv_t *priv) {
  if (priv->rom_data != nullptr) {
    free(priv->rom_data);
    priv->rom_data = nullptr;
  }
  if (priv->cart_ram != nullptr) {
    free(priv->cart_ram);
    priv->cart_ram = nullptr;
  }
  if (priv->rom_cache != nullptr) {
    free(priv->rom_cache);
    priv->rom_cache = nullptr;
  }
  if (priv->rom_file) {
    priv->rom_file.close();
  }
}

#if ENABLE_LCD
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160],
                   const uint_fast8_t line) {
  int yplot = line * DEST_H / LCD_HEIGHT;
  uint16_t(*fb565)[LCD_WIDTH] = ((priv_t *)gb->direct.priv)->fb;

#if WALNUT_FULL_GBC_SUPPORT
  if (gb->cgb.cgbMode) {
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb565[yplot][x] = gb->cgb.fixPalette[pixels[x]];
    }
  } else
#endif
  {
#if WALNUT_GB_12_COLOUR
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb565[yplot][x] =
          CURRENT_PALETTE_RGB565[((pixels[x] & 18) >> 1) | (pixels[x] & 3)];
    }
#else
    for (unsigned int x = 0; x < LCD_WIDTH; x++) {
      fb565[yplot][x] = CURRENT_PALETTE_RGB565[(pixels[x]) & 3];
    }
#endif
  }
}

void fit_frame(uint16_t fb[144][160]) {
  M5Cardputer.Display.drawBitmap(40, 0, 160, 135, fb[0]);
}
#endif

bool init_sd_card() {
  return mount_sd_card(true);
}

void map_game_input(struct gb_s *gb, bool *exit_requested,
                    bool *frame_skip_pressed) {
  gb->direct.joypad = 0xFF;
  *frame_skip_pressed = false;

  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isPressed()) {
    return;
  }

  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
  if (status.fn && status.del) {
    *exit_requested = true;
    return;
  }

  if (status.enter) {
    gb->direct.joypad_bits.start = 0;
  }
  if (status.space) {
    gb->direct.joypad_bits.a = 0;
  }
  if (status.tab) {
    gb->direct.joypad_bits.select = 0;
  }

  for (char key : status.word) {
    switch (key) {
      case 'e':
      case 'w':
      case ';':
        gb->direct.joypad_bits.up = 0;
        break;
      case 'a':
        gb->direct.joypad_bits.left = 0;
        break;
      case 's':
      case '.':
        gb->direct.joypad_bits.down = 0;
        break;
      case 'd':
        gb->direct.joypad_bits.right = 0;
        break;
      case 'k':
      case 'j':
      case ',':
        gb->direct.joypad_bits.b = 0;
        break;
      case 'l':
      case 'i':
      case '/':
        gb->direct.joypad_bits.a = 0;
        break;
      case '1':
      case 'p':
        gb->direct.joypad_bits.start = 0;
        break;
      case '2':
      case '0':
      case 'o':
        gb->direct.joypad_bits.select = 0;
        break;
      case '3':
      case 'f':
        *frame_skip_pressed = true;
        break;
      default:
        break;
    }
  }
}

void reset_perf_stats(PerfStats *perf) {
  memset(perf, 0, sizeof(*perf));
  perf->report_start_ms = millis();
  perf->sample_start_ms = perf->report_start_ms;
}

void report_perf(const priv_t *priv, const PerfStats *perf,
                 bool frame_skip_enabled) {
  if (perf->frames == 0) {
    return;
  }

  const uint32_t elapsed_ms = millis() - perf->report_start_ms;
  const double fps =
      elapsed_ms == 0 ? 0.0 : (double)perf->frames * 1000.0 / elapsed_ms;
  const uint32_t avg_frame_us = perf->frame_work_us / perf->frames;
  const uint32_t avg_emu_us = perf->emu_us / perf->frames;
  const uint32_t avg_draw_us = perf->draw_us / perf->frames;

  Serial.printf("[perf] fps=%.1f avg_us=%u emu_us=%u draw_us=%u backing=%s "
                "cache_h=%u cache_m=%u frame_skip=%s free_heap=%u\n",
                fps, (unsigned int)avg_frame_us, (unsigned int)avg_emu_us,
                (unsigned int)avg_draw_us, priv->rom_in_ram ? "ram" : "sd",
                (unsigned int)priv->rom_cache_hits,
                (unsigned int)priv->rom_cache_misses,
                frame_skip_enabled ? "on" : "off",
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void run_game(const char *rom_path) {
  static struct gb_s gb;
  static struct priv_t priv;
  memset(&gb, 0, sizeof(gb));
  memset(&priv, 0, sizeof(priv));

  show_message("Loading", base_name(rom_path));
  char error[NAME_SIZE] = {0};
  if (!open_rom_cache(&priv, rom_path, error, sizeof(error))) {
    show_message("ROM failed", error, "Enter: launcher");
    wait_for_enter();
    free_game(&priv);
    return;
  }

  enum gb_init_error_e ret =
      gb_init(&gb, &gb_rom_read, &gb_rom_read_16bit, &gb_rom_read_32bit,
              &gb_cart_ram_read, &gb_cart_ram_write, &gb_error, &priv);
  if (ret != GB_INIT_NO_ERROR) {
    show_message("GB init failed", gb_init_error_name(ret), "Enter: launcher");
    wait_for_enter();
    free_game(&priv);
    return;
  }

  if (gb_get_save_size_s(&gb, &priv.cart_ram_size) != 0) {
    priv.cart_ram_size = gb_get_save_size(&gb);
  }

  if (priv.cart_ram_size > 0) {
    priv.cart_ram = (uint8_t *)malloc(priv.cart_ram_size);
    if (priv.cart_ram == nullptr) {
      show_message("Save RAM failed", "Not enough heap", "Enter: launcher");
      wait_for_enter();
      free_game(&priv);
      return;
    }

    make_save_path(rom_path, priv.save_path, sizeof(priv.save_path));
    load_save(&priv);
  }

#if ENABLE_LCD
  gb_init_lcd(&gb, &lcd_draw_line);
  gb.direct.interlace = 0;
#if GBPUTER_FAST_DMG
  gb.direct.frame_skip = false;
#endif
#endif

  M5Cardputer.Display.clearDisplay();
  const uint32_t target_speed_us = (uint32_t)(1000000.0 / VERTICAL_SYNC);
  unsigned long last_save_ms = millis();
  PerfStats perf;
  reset_perf_stats(&perf);
  bool exit_requested = false;
  bool frame_skip_key_down = false;

  while (!exit_requested) {
    int32_t frame_delay;
    uint32_t emu_elapsed = 0;
    uint32_t draw_elapsed = 0;
    const uint32_t frame_start = micros();

    bool frame_skip_pressed = false;
    map_game_input(&gb, &exit_requested, &frame_skip_pressed);
    if (exit_requested) {
      break;
    }

    if (frame_skip_pressed && !frame_skip_key_down) {
      gb.direct.frame_skip = !gb.direct.frame_skip;
      perf.frame_skip_checked = true;
      Serial.printf("[perf] frame_skip=%s manual\n",
                    gb.direct.frame_skip ? "on" : "off");
    }
    frame_skip_key_down = frame_skip_pressed;

    const uint32_t emu_start = micros();
    gb_run_frame_dualfetch(&gb);
    emu_elapsed = micros() - emu_start;

#if ENABLE_LCD
    const uint32_t draw_start = micros();
    fit_frame(priv.fb);
    draw_elapsed = micros() - draw_start;
#endif

    if (priv.error_text[0] != '\0') {
      show_message("Emulator stopped", priv.error_text, "Enter: launcher");
      wait_for_enter();
      break;
    }

    if (millis() - last_save_ms > 2500) {
      flush_save(&priv);
      last_save_ms = millis();
    }

    const uint32_t frame_work_us = micros() - frame_start;
    perf.frames++;
    perf.frame_work_us += frame_work_us;
    perf.emu_us += emu_elapsed;
    perf.draw_us += draw_elapsed;

#if GBPUTER_FAST_DMG
    if (!perf.frame_skip_checked &&
        millis() - perf.sample_start_ms >= FRAME_SKIP_SAMPLE_MS &&
        perf.frames > 0) {
      const uint32_t avg_frame_us = perf.frame_work_us / perf.frames;
      if (avg_frame_us > target_speed_us) {
        gb.direct.frame_skip = true;
        Serial.printf("[perf] frame_skip=on auto avg_us=%u budget_us=%u\n",
                      (unsigned int)avg_frame_us,
                      (unsigned int)target_speed_us);
      }
      perf.frame_skip_checked = true;
    }
#endif

    if (millis() - perf.report_start_ms >= PERF_REPORT_INTERVAL_MS) {
      report_perf(&priv, &perf, gb.direct.frame_skip);
      const bool frame_skip_checked = perf.frame_skip_checked;
      reset_perf_stats(&perf);
      perf.frame_skip_checked = frame_skip_checked;
    }

    frame_delay = (int32_t)target_speed_us - (int32_t)frame_work_us;
    if (frame_delay > 0) {
      delayMicroseconds(frame_delay);
    }
  }

  bool save_ok = flush_save(&priv);
  free_game(&priv);
  wait_for_key_release();

  if (!save_ok) {
    show_message("Save failed", "Could not write .sav", "Enter: launcher");
    wait_for_enter();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\n[boot] Gameboy-Puter mode=%s\n",
                GBPUTER_FAST_DMG ? "fast-dmg" : "compat");
  update_palette();
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

#if ENABLE_SOUND
  M5Cardputer.Speaker.begin();
#endif

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  set_font_size(100);
  init_sd_card();
}

void loop() {
  char selected_path[PATH_SIZE] = {0};
  if (pick_rom(selected_path, sizeof(selected_path))) {
    run_game(selected_path);
  }
}
