#include "cdrom_io.h"
#include "osd.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstdlib>

#ifndef CDROM_DRIVE_STATUS
#define CDROM_DRIVE_STATUS 0x5326
#endif
#ifndef CDS_TRAY_OPEN
#define CDS_TRAY_OPEN 2
#endif
#ifndef CDS_DISC_OK
#define CDS_DISC_OK 4
#endif

// Variáveis globais
static CDROMState cdrom_states[4] = {};
static bool monitoring_active = false;
static pthread_t monitor_thread;
static CDROMStatusCallback active_callback = nullptr;
static pthread_mutex_t monitor_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread de leitura de TOC
static pthread_t toc_threads[4] = {};
static bool toc_thread_active[4] = {false, false, false, false};
static pthread_mutex_t toc_mutex = PTHREAD_MUTEX_INITIALIZER;

// Logging Helper
void log_debug(const char *fmt, ...) {
  FILE *f = fopen("/media/fat/cdrom_debug.log", "a");
  if (f) {
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    va_list args;
    va_start(args, fmt);
    fprintf(f, "[%s] ", tbuf);
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
    va_end(args);
    fclose(f);
  }
  // Also print to console
  va_list args2;
  va_start(args2, fmt);
  vprintf(fmt, args2);
  printf("\n");
  va_end(args2);
}

// Helper to identify disc type
static PhysicalDiscType identify_disc(int fd) {
  unsigned char buffer[2352]; // Buffer for one sector
  PhysicalDiscType type = DISC_UNKNOWN;

  // 1. Check Sector 0 (LBA 0) for SEGA/SATURN/NEO-GEO
  lseek(fd, 0, SEEK_SET);
  if (read(fd, buffer, 2048) > 0) {
    if (memcmp(buffer, "SEGADISCSYSTEM", 14) == 0)
      type = DISC_MEGACD;
    else if (memcmp(buffer, "SEGA SEGASATURN", 15) == 0)
      type = DISC_SATURN;
  }

  if (type != DISC_UNKNOWN)
    return type;

  // 2. Check Sector 16 (LBA 16) - ISO9660 Header
  lseek(fd, 16 * 2048, SEEK_SET);
  if (read(fd, buffer, 2048) > 0) {
    if (memcmp(buffer + 1, "CD001", 5) == 0) { // ISO9660
      // Check system identifier in ISO header (offset 8)
      if (memcmp(buffer + 8, "SEGADISCSYSTEM", 14) == 0)
        type = DISC_MEGACD;
      else if (memcmp(buffer + 8, "SEGA SEGASATURN", 15) == 0)
        type = DISC_SATURN;
    }
  }

  if (type != DISC_UNKNOWN)
    return type;

  // 3. Check PSX License at Sector 4
  lseek(fd, 4 * 2048, SEEK_SET);
  if (read(fd, buffer, 2352) > 0) {
    if (memmem(buffer, 2048, "Sony Computer Entertainment", 27))
      return DISC_PSX;
  }

  return DISC_UNKNOWN;
}

// Estrutura para passar dados para a thread de TOC
struct TOCThreadData {
  int index;
  char path[32];
};

// Thread para ler TOC em background
static void* toc_reader_thread(void* arg) {
  TOCThreadData* data = (TOCThreadData*)arg;
  int index = data->index;
  char path[32];
  strcpy(path, data->path);
  free(data);

  log_debug("[TOC Thread %d] Starting background TOC read for %s", index, path);

  // 1. Identificar tipo de disco
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    log_debug("[TOC Thread %d] Failed to open device: %s", index, strerror(errno));
    pthread_mutex_lock(&toc_mutex);
    toc_thread_active[index] = false;
    pthread_mutex_unlock(&toc_mutex);
    return NULL;
  }

  PhysicalDiscType disc_type = identify_disc(fd);
  close(fd);

  pthread_mutex_lock(&monitor_mutex);
  cdrom_states[index].disc_type = disc_type;
  pthread_mutex_unlock(&monitor_mutex);

  log_debug("[TOC Thread %d] Disc type identified: %d", index, disc_type);

  // 2. Ler TOC (se tipo conhecido)
  if (disc_type != DISC_UNKNOWN) {
    CDROM_TrackInfo tracks[100];
    int track_count = read_cdrom_toc(index, tracks, 100);
    log_debug("[TOC Thread %d] TOC read complete. Tracks: %d", index, track_count);
  }

  // 3. Marcar TOC como pronto
  pthread_mutex_lock(&monitor_mutex);
  cdrom_states[index].toc_ready = true;
  pthread_mutex_unlock(&monitor_mutex);

  pthread_mutex_lock(&toc_mutex);
  toc_thread_active[index] = false;
  pthread_mutex_unlock(&toc_mutex);

  log_debug("[TOC Thread %d] Completed successfully", index);
  return NULL;
}

// Verifica mudanças no estado de um CD-ROM específico
bool check_cdrom_state(int index) {
  char path[32];
  sprintf(path, "/dev/sr%d", index);

  struct stat st;
  int stat_res = stat(path, &st);
  bool is_block = (stat_res == 0 && S_ISBLK(st.st_mode));
  bool currently_present = is_block;
  bool current_media = false;
  bool current_tray_open = false;

  if (currently_present) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      int status = ioctl(fd, CDROM_DRIVE_STATUS, 0);
      if (status == CDS_DISC_OK) {
        current_media = true;
        current_tray_open = false;
        // NÃO identificar aqui - será feito em background
      } else if (status == CDS_TRAY_OPEN) {
        current_media = false;
        current_tray_open = true;
        cdrom_states[index].disc_type = DISC_UNKNOWN;
        cdrom_states[index].toc_ready = false;
      } else {
        current_tray_open = false;
        cdrom_states[index].disc_type = DISC_UNKNOWN;
        cdrom_states[index].toc_ready = false;
      }
      close(fd);
    }
  } else {
    cdrom_states[index].disc_type = DISC_UNKNOWN;
    cdrom_states[index].toc_ready = false;
    current_tray_open = false;
  }

  if (currently_present != cdrom_states[index].present ||
      current_media != cdrom_states[index].media_present ||
      current_tray_open != cdrom_states[index].tray_open) {

    cdrom_states[index].present = currently_present;
    cdrom_states[index].media_present = current_media;
    cdrom_states[index].tray_open = current_tray_open;
    strcpy(cdrom_states[index].path, path);

    // Se novo disco detectado, iniciar thread de TOC
    if (current_media && !cdrom_states[index].toc_ready) {
      pthread_mutex_lock(&toc_mutex);
      // Cancelar thread anterior se ainda ativa
      if (toc_thread_active[index]) {
        pthread_cancel(toc_threads[index]);
        pthread_join(toc_threads[index], NULL);
        toc_thread_active[index] = false;
      }
      pthread_mutex_unlock(&toc_mutex);

      // Resetar estado
      cdrom_states[index].disc_type = DISC_UNKNOWN;
      cdrom_states[index].toc_ready = false;

      // Criar thread para ler TOC
      TOCThreadData* data = (TOCThreadData*)malloc(sizeof(TOCThreadData));
      data->index = index;
      strcpy(data->path, path);

      pthread_mutex_lock(&toc_mutex);
      toc_thread_active[index] = true;
      pthread_create(&toc_threads[index], NULL, toc_reader_thread, data);
      pthread_detach(toc_threads[index]);  // Detach para auto-cleanup
      pthread_mutex_unlock(&toc_mutex);

      log_debug("[CDROM %d] Started background TOC reading thread", index);
    }

    return true;
  }

  return false;
}

// Thread de monitoramento
static void *cdrom_monitor_thread(void *arg) {
  (void)arg;
  printf("[CDROM] Monitor Thread Started\n");

  while (monitoring_active) {
    for (int i = 0; i < 4; i++) {
      if (check_cdrom_state(i)) {
        pthread_mutex_lock(&monitor_mutex);
        if (active_callback) {
          active_callback(i, cdrom_states[i].present);
        }
        pthread_mutex_unlock(&monitor_mutex);
      }
    }
    sleep(CHECK_INTERVAL);
  }
  printf("[CDROM] Monitor Thread Stopped\n");
  return NULL;
}

// Iniciar monitoramento
void startCDROMMonitoring(CDROMStatusCallback callback) {
  pthread_mutex_lock(&monitor_mutex);
  if (!monitoring_active) {
    active_callback = callback;
    monitoring_active = true;
    pthread_create(&monitor_thread, NULL, cdrom_monitor_thread, NULL);
  }
  pthread_mutex_unlock(&monitor_mutex);
}

// Parar monitoramento
void stopCDROMMonitoring() {
  pthread_mutex_lock(&monitor_mutex);
  if (monitoring_active) {
    monitoring_active = false;
    pthread_mutex_unlock(&monitor_mutex);
    pthread_join(monitor_thread, NULL);
    pthread_mutex_lock(&monitor_mutex);
    active_callback = nullptr;
  }
  pthread_mutex_unlock(&monitor_mutex);
}

bool isCDROMTrayOpen(int index) {
  if (index < 0 || index >= 4)
    return false;
  return cdrom_states[index].tray_open;
}

bool isCDROMPresent(int index) {
  if (index < 0 || index >= 4)
    return false;
  return cdrom_states[index].present;
}

bool hasCDROMMedia(int index) {
  if (index < 0 || index >= 4)
    return false;
  return cdrom_states[index].media_present;
}

PhysicalDiscType getCDROMType(int index) {
  if (index < 0 || index >= 4)
    return DISC_UNKNOWN;
  return cdrom_states[index].disc_type;
}

bool isCDROMTocReady(int index) {
  if (index < 0 || index >= 4)
    return false;
  return cdrom_states[index].toc_ready;
}

// Read raw sector from CD-ROM
int read_cdrom_sector(int index, int lba, unsigned char *buffer,
                      int sector_size) {
  if (index < 0 || index >= 4 || !cdrom_states[index].media_present)
    return 0;

  // log_debug("MCD: Read Sector LBA=%d Size=%d", lba, sector_size);

  int fd = open(cdrom_states[index].path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    log_debug("MCD: Error opening device %s for read (LBA=%d): %s",
              cdrom_states[index].path, lba, strerror(errno));
    return 0;
  }

  // Check for Audio Read (multiples of 2352 bytes)
  if (sector_size > 0 && (sector_size % 2352 == 0)) {
    struct cdrom_read_audio ra;
    ra.addr.lba = lba;
    ra.addr_format = CDROM_LBA;
    ra.nframes = sector_size / 2352;
    ra.buf = buffer;

    int res = ioctl(fd, CDROMREADAUDIO, &ra);
    close(fd);

    if (res < 0) {
      log_debug("MCD: CDROMREADAUDIO Error LBA=%d Frames=%d: %s", lba,
                ra.nframes, strerror(errno));
      return 0; // Return 0 (silence) on error
    }
    return sector_size;
  }

  // Standard data read for now.
  lseek(fd, (off_t)lba * 2048, SEEK_SET);

  int bytes_read = 0;
  int retry = 3000;
  while (retry > 0) {
    bytes_read = read(fd, buffer, sector_size > 2048 ? 2048 : sector_size);
    if (bytes_read < 0) {
      if (errno == EAGAIN || errno == EBUSY) {
        usleep(1000); // 1ms
        retry--;
        continue;
      }
      log_debug("MCD: Read Error LBA=%d: %s", lba, strerror(errno));
      break;
    }
    break;
  }

  close(fd);

  if (bytes_read <= 0)
    return 0;
  return bytes_read;
}

// Read TOC from physical CD
int read_cdrom_toc(int index, CDROM_TrackInfo *tracks, int max_tracks) {
  if (index < 0 || index >= 4 || !cdrom_states[index].media_present)
    return 0;

  int fd = open(cdrom_states[index].path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return 0;

  struct cdrom_tochdr input_header;
  if (ioctl(fd, CDROMREADTOCHDR, &input_header) < 0) {
    close(fd);
    return 0;
  }

  int count = 0;
  int start_track = input_header.cdth_trk0;
  int end_track = input_header.cdth_trk1;

  for (int i = start_track; i <= end_track && count < max_tracks; i++) {
    struct cdrom_tocentry entry;
    entry.cdte_track = i;
    entry.cdte_format = CDROM_LBA;
    if (ioctl(fd, CDROMREADTOCENTRY, &entry) < 0)
      continue;

    tracks[count].start_lba = entry.cdte_addr.lba;

    // Type: 4=Data, others=Audio (Control field: bit 2 is data)
    tracks[count].type = (entry.cdte_ctrl & CDROM_DATA_TRACK) ? 1 : 0;

    // Determine end (start of next track)
    if (i < end_track) {
      struct cdrom_tocentry next;
      next.cdte_track = i + 1;
      next.cdte_format = CDROM_LBA;
      if (ioctl(fd, CDROMREADTOCENTRY, &next) == 0) {
        tracks[count].end_lba = next.cdte_addr.lba - 1;
      } else {
        tracks[count].end_lba = tracks[count].start_lba;
      }
    } else {
      struct cdrom_tocentry leadout;
      leadout.cdte_track = CDROM_LEADOUT;
      leadout.cdte_format = CDROM_LBA;
      if (ioctl(fd, CDROMREADTOCENTRY, &leadout) == 0) {
        tracks[count].end_lba = leadout.cdte_addr.lba - 1;
      }
    }

    count++;
  }

  log_debug("MCD: TOC Read. Tracks: %d", count);
  close(fd);
  return count;
}
// Função para ejetar CD físico
int eject_cdrom(int index) {
  if (index < 0 || index >= 4) {
    log_debug("eject_cdrom: Invalid index %d", index);
    printf("[EJECT] eject_cdrom called with index=%d\n", index);
    return -1;
  }

  char device[32];
  snprintf(device, sizeof(device), "/dev/sr%d", index);
  
  log_debug("Attempting to eject CD from %s", device);
  printf("[EJECT] eject_cdrom called with index=%d device=%s\n", index, device);
  
  int fd = open(device, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    log_debug("eject_cdrom: Failed to open %s: %s", device, strerror(errno));
    printf("[EJECT] ERROR: Failed to open %s: %s\n", device, strerror(errno));
    return -1;
  }
  
  // Try to unlock the drive first (in case it's locked)
  ioctl(fd, CDROM_LOCKDOOR, 0);
  
  // Small delay to let any pending operations complete
  usleep(100000); // 100ms
  
  // Eject the CD using ioctl
  int result = ioctl(fd, CDROMEJECT);
  if (result < 0) {
    log_debug("eject_cdrom: CDROMEJECT failed for %s: %s", device, strerror(errno));
    printf("[EJECT] ERROR: CDROMEJECT failed for %s: %s\n", device, strerror(errno));
    
    // If busy, wait a bit and retry
    if (errno == EBUSY) {
      printf("[EJECT] Device busy, waiting and retrying...\n");
      close(fd);
      usleep(500000); // 500ms
      
      // Retry
      fd = open(device, O_RDONLY | O_NONBLOCK);
      if (fd >= 0) {
        ioctl(fd, CDROM_LOCKDOOR, 0);
        usleep(100000);
        result = ioctl(fd, CDROMEJECT);
        if (result < 0) {
          log_debug("eject_cdrom: Retry also failed: %s", strerror(errno));
          printf("[EJECT] ERROR: Retry also failed: %s\n", strerror(errno));
        }
      }
    }
  }
  
  if (result >= 0) {
    log_debug("eject_cdrom: Successfully ejected CD from %s", device);
    printf("[EJECT] SUCCESS! CD ejected from %s\n", device);
    
    // Update state to reflect tray is now open
    pthread_mutex_lock(&monitor_mutex);
    cdrom_states[index].media_present = false;
    cdrom_states[index].tray_open = true;
    cdrom_states[index].disc_type = DISC_UNKNOWN;
    cdrom_states[index].toc_ready = false;
    pthread_mutex_unlock(&monitor_mutex);
  }
  
  close(fd);
  return result;
}
