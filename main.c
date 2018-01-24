#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <ctype.h>

#include <Eina.h>

#include "keymap.h"

static int fd;
static struct uinput_user_dev uidev;
static struct input_event ev;

static Eina_Hash *_keys_map = NULL;

#define check_ret(ret) do{\
     if (ret < 0) {\
          printf("Error at %s:%d\n", __func__, __LINE__);\
          exit(EXIT_FAILURE);\
     }\
} while(0)

static void
_configure_dev()
{
   char *str = alloca(100);
   int ret, i;
   memset(&uidev, 0, sizeof(uidev));

   fd = open("/dev/uinput", O_WRONLY);
   check_ret(fd);

   snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "uinput-sample");

   uidev.id.bustype = BUS_USB;
   uidev.id.vendor  = 1;
   uidev.id.product = 1;
   uidev.id.version = 1;

   ret = write(fd, &uidev, sizeof(uidev));
   if (ret != sizeof(uidev)) {
        printf("Failed to write dev structure\n");
        exit(EXIT_FAILURE);
   }

   ret = ioctl(fd, UI_SET_EVBIT, EV_KEY);
   check_ret(ret);

   _keys_map = eina_hash_string_superfast_new(NULL);
   for(i=0;i<sizeof(kmap)/sizeof(*kmap);i++) {
        memcpy(str, kmap[i].string, strlen(kmap[i].string) + 1);
        eina_str_tolower(&str);
        eina_hash_set(_keys_map, str, &(kmap[i]));
        ret = ioctl(fd, UI_SET_KEYBIT, kmap[i].kernelcode);
        check_ret(ret);
   }

   ret = ioctl(fd, UI_DEV_CREATE);
   check_ret(ret);
}

static void
_send_event(__u16 type, __u16 code, __s32 value)
{
   int ret;
   memset(&ev, 0, sizeof(ev));

   ev.type = type;
   ev.code = code;
   ev.value = value;

   ret = write(fd, &ev, sizeof(ev));
   check_ret(ret);
}

static void
_send_key(int key, int state)
{
   _send_event(EV_KEY, key, state);
   usleep(50000);
   _send_event(EV_SYN, SYN_REPORT, 0);
   usleep(50000);
}

static int
_key_find_from_char(char c)
{
   char str[2];
   str[0] = tolower(c);
   str[1] = '\0';
   struct map *key = eina_hash_find(_keys_map, str);
   if (!key)
     {
        fprintf(stderr, "Key not found for %c\n", c);
        return -1;
     }
   return key->kernelcode;
}

static int
_key_find_from_string(const char *string, int len)
{
   char *str = alloca(len + 1);
   memcpy(str, string, len);
   str[len] = '\0';
   eina_str_tolower(&str);
   struct map *key = eina_hash_find(_keys_map, str);
   if (!key)
     {
        fprintf(stderr, "Key not found for %s\n", str);
        return -1;
     }
   return key->kernelcode;
}

static char *
_file_get_as_string(const char *filename)
{
   char *file_data = NULL;
   int file_size;
   FILE *fp = fopen(filename, "rb");
   if (!fp)
     {
        fprintf(stderr, "Can not open file: \"%s\".", filename);
        return NULL;
     }

   fseek(fp, 0, SEEK_END);
   file_size = ftell(fp);
   if (file_size == -1)
     {
        fclose(fp);
        fprintf(stderr, "Can not ftell file: \"%s\".", filename);
        return NULL;
     }
   rewind(fp);
   file_data = (char *) calloc(1, file_size + 1);
   if (!file_data)
     {
        fclose(fp);
        fprintf(stderr, "Calloc failed");
        return NULL;
     }
   int res = fread(file_data, file_size, 1, fp);
   fclose(fp);
   if (!res)
     {
        free(file_data);
        file_data = NULL;
        fprintf(stderr, "fread failed");
     }
   return file_data;
}

int main(int argc, char** argv)
{
   if (argc != 2)
     {
        fprintf(stderr, "Need at least one file\n");
        exit(-1);
     }
   _configure_dev();
   usleep(50000);

   char *buf = _file_get_as_string(argv[1]);
   if (!buf)
     {
        fprintf(stderr, "Issue while accessing %s\n", argv[1]);
        exit(1);
     }

#define WSKIP while (*buf == ' ') buf++;

   while (*buf)
     {
        int down = 1;
        WSKIP;
        if (*buf == '\n') buf++;
        if (!strncmp(buf, "KEY ", 4))
          {
             buf += 3;
             while (*buf == ' ')
               {
                  WSKIP;
                  char *end = buf;
                  while (*end && *end != '\n' && *end != ' ') end++;
                  int key = _key_find_from_string(buf, end - buf);
                  _send_key(key, 1);
                  _send_key(key, 0);
                  buf = end;
               }
          }
        else if (!strncmp(buf, "TYPE ", 5))
          {
             buf += 5;
             while (*buf != '\n')
               {
                  char c = *buf;
                  int key = _key_find_from_char(c);
                  _send_key(key, 1);
                  _send_key(key, 0);
                  buf++;
               }
          }
        else if ((down = !strncmp(buf, "KEY_DOWN ", 9)) || !strncmp(buf, "KEY_UP ", 7))
          {
             buf += (down ? 8 : 6);
             while (*buf == ' ')
               {
                  WSKIP;
                  char *end = buf;
                  while (*end && *end != '\n' && *end != ' ') end++;
                  _send_key(_key_find_from_string(buf, end - buf), down ? 1 : 0);
                  buf = end;
               }
          }
        else if (!strncmp(buf, "DELAY ", 6))
          {
             char *end = NULL;
             buf += 6;
             int d = strtol(buf, &end, 10);
             buf = end;
             WSKIP;
             if (*buf && *buf != '\n')
               {
                  fprintf(stderr, "DELAY expects an integer representing milliseconds\n");
                  exit(1);
               }
             usleep(d * 1000);
          }
        else
          {
             fprintf(stderr, "Unknown token: %s\n", buf);
             exit(1);
          }
     }
   return 0;
   _send_key(KEY_LEFTALT, 1);
   _send_key(KEY_ESC, 1);
   _send_key(KEY_LEFTALT, 0);
   _send_key(KEY_ESC, 0);
  sleep(1);
   _send_key(KEY_F, 1);
   _send_key(KEY_F, 0);
   _send_key(KEY_ENTER, 1);
   _send_key(KEY_ENTER, 0);
   sleep(2);
   _send_key(KEY_A, 1);

   return 0;
}

