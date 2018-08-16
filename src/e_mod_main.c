#define EFL_BETA_API_SUPPORT
#define EFL_EO_API_SUPPORT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <ctype.h>
#include <syslog.h>

#ifndef STAND_ALONE
#include <e.h>
#else
#include <Elementary.h>
#endif
#include <Eina.h>
#include <Ecore.h>
#include <Ecore_Con.h>

#include "e_mod_main.h"
#include "keymap.h"

#define _EET_ENTRY "config"

#define DELAY 0.01

typedef struct
{
#ifndef STAND_ALONE
   E_Gadcon_Client *gcc;
   E_Gadcon_Popup *popup;
#endif
   Evas_Object *o_icon;
   Eo *main_box;

   Eina_List *items;
   Ecore_File_Monitor *config_dir_monitor;
   Eina_Stringshare *cfg_path;

   int fd;
   struct input_event ev;
   Eina_Hash *keys_map;
} Instance;

#define PRINT(fmt, ...) \
{ \
   char pbuf[1000]; \
   sprintf(pbuf, fmt"\n", ## __VA_ARGS__); \
   syslog(LOG_NOTICE, pbuf); \
}

#ifndef STAND_ALONE
static E_Module *_module = NULL;
#endif

#define check_ret(ret) do{\
     if (ret < 0) {\
          PRINT("Error at %s:%d", __func__, __LINE__);\
          return EINA_FALSE; \
     }\
} while(0)

typedef struct
{
   Instance *instance;
   Ecore_Timer *timer;
   Eina_Stringshare *filename;
   Eina_Stringshare *name;
   Eo *start_button;
   Eina_Bool playing;
   char *filedata;
   char *cur_filedata;
   char *cur_state;
} Item_Desc;

static void _start_stop_bt_clicked(void *data, Evas_Object *obj, void *event_info);

static Eina_Bool
_configure_dev(Instance *inst)
{
   struct uinput_user_dev uidev;
   char *str = alloca(100);
   int ret;
   unsigned int i;
   memset(&uidev, 0, sizeof(uidev));

   inst->fd = open("/dev/uinput", O_WRONLY);
   check_ret(inst->fd);

   snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "uinput-sample");

   uidev.id.bustype = BUS_USB;
   uidev.id.vendor  = 1;
   uidev.id.product = 1;
   uidev.id.version = 1;

   ret = write(inst->fd, &uidev, sizeof(uidev));
   if (ret != sizeof(uidev)) {
        PRINT("Failed to write dev structure");
        return EINA_FALSE;
   }

   ret = ioctl(inst->fd, UI_SET_EVBIT, EV_KEY);
   check_ret(ret);

   inst->keys_map = eina_hash_string_superfast_new(NULL);
   for(i = 0; i < sizeof(kmap) / sizeof(*kmap); i++) {
        memcpy(str, kmap[i].string, strlen(kmap[i].string) + 1);
        eina_str_tolower(&str);
        eina_hash_set(inst->keys_map, str, &(kmap[i]));
        ret = ioctl(inst->fd, UI_SET_KEYBIT, kmap[i].kernelcode);
        check_ret(ret);
   }

   ret = ioctl(inst->fd, UI_DEV_CREATE);
   check_ret(ret);
   PRINT("Init done");
   return EINA_TRUE;
}

static Eina_Bool
_send_event(Instance *inst, __u16 type, __u16 code, __s32 value)
{
   int ret;
   memset(&inst->ev, 0, sizeof(inst->ev));

   inst->ev.type = type;
   inst->ev.code = code;
   inst->ev.value = value;

   ret = write(inst->fd, &inst->ev, sizeof(inst->ev));
   check_ret(ret);
   return EINA_TRUE;
}

static Eina_Bool _consume(void *data);

static void
_send_key(Item_Desc *idesc, int key, int state)
{
   _send_event(idesc->instance, EV_KEY, key, state);
   _send_event(idesc->instance, EV_SYN, SYN_REPORT, 0);
}

static int
_key_find_from_char(Instance *inst, char c)
{
   char str[2];
   str[0] = tolower(c);
   str[1] = '\0';
   struct map *key = eina_hash_find(inst->keys_map, str);
   if (!key)
     {
        PRINT("Key not found for %c", c);
        return -1;
     }
   return key->kernelcode;
}

static int
_key_find_from_string(Instance *inst, const char *string, int len)
{
   char *str = alloca(len + 1);
   memcpy(str, string, len);
   str[len] = '\0';
   eina_str_tolower(&str);
   struct map *key = eina_hash_find(inst->keys_map, str);
   if (!key)
     {
        PRINT("Key not found for %s", str);
        return -1;
     }
   return key->kernelcode;
}

#define WSKIP while (*idesc->cur_filedata == ' ') idesc->cur_filedata++;

static Eina_Bool
_consume(void *data)
{
   Item_Desc *idesc = data;
   Instance *inst = idesc->instance;
   if (*idesc->cur_filedata)
     {
        int down = 1;
        while (*idesc->cur_filedata == '\n')
          {
             idesc->cur_filedata++;
             idesc->cur_state = NULL;
             WSKIP;
          }
        if (!strncmp(idesc->cur_state ? idesc->cur_state : idesc->cur_filedata, "KEY ", 4))
          {
             if (!idesc->cur_state)
               {
                  idesc->cur_state = idesc->cur_filedata;
                  idesc->cur_filedata += 3;
               }
             if (*idesc->cur_filedata == ' ')
               {
                  WSKIP;
                  char *end = idesc->cur_filedata;
                  while (*end && *end != '\n' && *end != ' ') end++;
                  int key = _key_find_from_string(inst, idesc->cur_filedata, end - idesc->cur_filedata);
                  idesc->cur_filedata = end;
                  _send_key(idesc, key, 1);
                  _send_key(idesc, key, 0);
                  idesc->timer = ecore_timer_add(DELAY, _consume, idesc);
                  PRINT("Key %d", key);
                  return EINA_FALSE;
               }
          }
        if (!strncmp(idesc->cur_state ? idesc->cur_state : idesc->cur_filedata, "TYPE ", 5))
          {
             if (!idesc->cur_state)
               {
                  idesc->cur_state = idesc->cur_filedata;
                  idesc->cur_filedata += 5;
               }
             char c = *idesc->cur_filedata;
             int key = _key_find_from_char(inst, c);
             idesc->cur_filedata++;
             _send_key(idesc, key, 1);
             _send_key(idesc, key, 0);
             idesc->timer = ecore_timer_add(DELAY, _consume, idesc);
             PRINT("Type %c", c);
             return EINA_FALSE;
          }
        else if ((down = !strncmp(idesc->cur_state ? idesc->cur_state : idesc->cur_filedata, "KEY_DOWN ", 9)) ||
              !strncmp(idesc->cur_state ? idesc->cur_state : idesc->cur_filedata, "KEY_UP ", 7))
          {
             if (!idesc->cur_state)
               {
                  idesc->cur_state = idesc->cur_filedata;
                  idesc->cur_filedata += (down ? 8 : 6);
               }
             if (*idesc->cur_filedata == ' ')
               {
                  WSKIP;
                  char *end = idesc->cur_filedata;
                  while (*end && *end != '\n' && *end != ' ') end++;
                  int key = _key_find_from_string(inst, idesc->cur_filedata, end - idesc->cur_filedata);
                  idesc->cur_filedata = end;
                  _send_key(idesc, key, down ? 1 : 0);
                  idesc->timer = ecore_timer_add(DELAY, _consume, idesc);
                  return EINA_FALSE;
               }
             PRINT("Key %s", down?"Down":"Up");
          }
        else if (!strncmp(idesc->cur_filedata, "DELAY ", 6))
          {
             char *end = NULL;
             idesc->cur_filedata += 6;
             int d = strtol(idesc->cur_filedata, &end, 10);
             idesc->cur_filedata = end;
             WSKIP;
             if (*idesc->cur_filedata && *idesc->cur_filedata != '\n')
               {
                  PRINT("DELAY expects an integer representing milliseconds");
                  return EINA_FALSE;
               }
             idesc->timer = ecore_timer_add(d / 1000.0, _consume, idesc);
             PRINT("Delay %dms", d);
             return EINA_FALSE;
          }
        else
          {
             if (*idesc->cur_filedata)
                PRINT("Unknown token: %s", idesc->cur_filedata);
          }
     }
   PRINT("Finishing consuming");
   _start_stop_bt_clicked(idesc, NULL, NULL);
   return EINA_FALSE;
}

#if 0
static Eo *
_label_create(Eo *parent, const char *text, Eo **wref)
{
   Eo *label = wref ? *wref : NULL;
   if (!label)
     {
        label = elm_label_add(parent);
        evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(label, EVAS_HINT_EXPAND, 0.0);
        evas_object_show(label);
        if (wref) efl_wref_add(label, wref);
     }
   elm_object_text_set(label, text);
   return label;
}
#endif

static Eo *
_button_create(Eo *parent, const char *text, Eo *icon, Eo **wref, Evas_Smart_Cb cb_func, void *cb_data)
{
   Eo *bt = wref ? *wref : NULL;
   if (!bt)
     {
        bt = elm_button_add(parent);
        evas_object_size_hint_align_set(bt, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(bt, 0.0, 0.0);
        evas_object_show(bt);
        if (wref) efl_wref_add(bt, wref);
        if (cb_func) evas_object_smart_callback_add(bt, "clicked", cb_func, cb_data);
     }
   elm_object_text_set(bt, text);
   elm_object_part_content_set(bt, "icon", icon);
   return bt;
}

static Eo *
_icon_create(Eo *parent, const char *path, Eo **wref)
{
   Eo *ic = wref ? *wref : NULL;
   if (!ic)
     {
        ic = elm_icon_add(parent);
        elm_icon_standard_set(ic, path);
        evas_object_show(ic);
        if (wref) efl_wref_add(ic, wref);
     }
   return ic;
}

static char *
_file_get_as_string(const char *filename)
{
   char *file_data = NULL;
   int file_size;
   FILE *fp = fopen(filename, "rb");
   if (!fp)
     {
        PRINT("Can not open file: \"%s\".", filename);
        return NULL;
     }

   fseek(fp, 0, SEEK_END);
   file_size = ftell(fp);
   if (file_size == -1)
     {
        fclose(fp);
        PRINT("Can not ftell file: \"%s\".", filename);
        return NULL;
     }
   rewind(fp);
   file_data = (char *) calloc(1, file_size + 1);
   if (!file_data)
     {
        fclose(fp);
        PRINT("Calloc failed");
        return NULL;
     }
   int res = fread(file_data, file_size, 1, fp);
   fclose(fp);
   if (!res)
     {
        free(file_data);
        file_data = NULL;
        PRINT("fread failed");
     }
   return file_data;
}

static void
_start_stop_bt_clicked(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   Eina_List *itr;
   Item_Desc *idesc = data, *idesc2;
   idesc->playing = !idesc->playing;
   elm_object_part_content_set(idesc->start_button, "icon",
      _icon_create(idesc->start_button,
         idesc->playing ? "media-playback-stop" : "media-playback-start", NULL));
   if (idesc->playing)
     {
        idesc->filedata = _file_get_as_string(idesc->filename);
        idesc->cur_filedata = idesc->filedata;
        idesc->cur_state = NULL;
        PRINT("Beginning consuming %s", idesc->filename);
        _consume(idesc);
     }
   else
     {
        free(idesc->filedata);
        idesc->filedata = idesc->cur_filedata = NULL;
        ecore_timer_del(idesc->timer);
        idesc->timer = NULL;
     }
   EINA_LIST_FOREACH(idesc->instance->items, itr, idesc2)
     {
        if (idesc2 != idesc)
           elm_object_disabled_set(idesc2->start_button, idesc->playing);
     }
}

static void
_box_update(Instance *inst, Eina_Bool clear)
{
   Eina_List *itr;
   Item_Desc *idesc;

   if (!inst->main_box) return;

   if (clear) elm_box_clear(inst->main_box);

   EINA_LIST_FOREACH(inst->items, itr, idesc)
     {
        Eo *b = elm_box_add(inst->main_box);
        elm_box_horizontal_set(b, EINA_TRUE);
        evas_object_size_hint_align_set(b, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(b, EVAS_HINT_EXPAND, 0.0);
        evas_object_show(b);
        elm_box_pack_end(inst->main_box, b);

        _button_create(b, idesc->name, NULL,
              &idesc->start_button, _start_stop_bt_clicked, idesc);
        evas_object_size_hint_weight_set(idesc->start_button, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
        elm_box_pack_end(b, idesc->start_button);
     }
}

static void
_config_dir_changed(void *data,
      Ecore_File_Monitor *em EINA_UNUSED,
      Ecore_File_Event event EINA_UNUSED, const char *_path EINA_UNUSED)
{
   Instance *inst = data;
   Eina_List *items = inst->items;
   Eina_List *l = ecore_file_ls(inst->cfg_path);
   char *file;
   Item_Desc *idesc;
   inst->items = NULL;
   EINA_LIST_FREE(l, file)
     {
        if (eina_str_has_suffix(file, ".seq"))
          {
             Eina_List *itr, *itr2;
             Eina_Bool found = EINA_FALSE;
             EINA_LIST_FOREACH_SAFE(items, itr, itr2, idesc)
               {
                  if (!found && !strncmp(file, idesc->name, strlen(file) - 4))
                    {
                       found = EINA_TRUE;
                       items = eina_list_remove(items, idesc);
                       inst->items = eina_list_append(inst->items, idesc);
                    }
               }
             if (!found)
               {
                  char path[1024];
                  sprintf(path, "%s/%s", inst->cfg_path, file);
                  idesc = calloc(1, sizeof(*idesc));
                  idesc->instance = inst;
                  idesc->filename = eina_stringshare_add(path);
                  idesc->name = eina_stringshare_add_length(file, strlen(file) - 4);
                  inst->items = eina_list_append(inst->items, idesc);
               }
          }
        free(file);
     }
   _box_update(inst, EINA_TRUE);
   EINA_LIST_FREE(items, idesc)
     {
        eina_stringshare_del(idesc->filename);
        free(idesc);
     }
}

static Eina_Bool
_mkdir(const char *dir)
{
   if (!ecore_file_exists(dir))
     {
        Eina_Bool success = ecore_file_mkdir(dir);
        if (!success)
          {
             PRINT("Cannot create a config folder \"%s\"", dir);
             return EINA_FALSE;
          }
     }
   return EINA_TRUE;
}

static Instance *
_instance_create()
{
   char path[1024];
   Instance *inst = calloc(1, sizeof(Instance));

   sprintf(path, "%s/e_kinjector", efreet_config_home_get());
   if (!_mkdir(path)) return NULL;
   inst->cfg_path = eina_stringshare_add(path);
   inst->config_dir_monitor = ecore_file_monitor_add(path, _config_dir_changed, inst);

   if (!_configure_dev(inst))
     {
        free(inst);
        inst = NULL;
     }
   return inst;
}

static void
_instance_delete(Instance *inst)
{
   if (inst->o_icon) evas_object_del(inst->o_icon);
   if (inst->main_box) evas_object_del(inst->main_box);

   free(inst);
}

#ifndef STAND_ALONE
static void
_popup_del(Instance *inst)
{
   E_FREE_FUNC(inst->popup, e_object_del);
}

static void
_popup_del_cb(void *obj)
{
   _popup_del(e_object_data_get(obj));
}

static void
_popup_comp_del_cb(void *data, Evas_Object *obj EINA_UNUSED)
{
   Instance *inst = data;

   E_FREE_FUNC(inst->popup, e_object_del);
}

static void
_button_cb_mouse_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Instance *inst;
   Evas_Event_Mouse_Down *ev;

   inst = data;
   ev = event_info;
   if (ev->button == 1)
     {
        if (!inst->popup)
          {
             Evas_Object *o;
             inst->popup = e_gadcon_popup_new(inst->gcc, 0);

             o = elm_box_add(e_comp->elm);
             evas_object_size_hint_align_set(o, EVAS_HINT_FILL, EVAS_HINT_FILL);
             evas_object_size_hint_weight_set(o, EVAS_HINT_EXPAND, 0.0);
             evas_object_show(o);
             efl_wref_add(o, &inst->main_box);

             _box_update(inst, EINA_FALSE);

             e_gadcon_popup_content_set(inst->popup, inst->main_box);
             e_comp_object_util_autoclose(inst->popup->comp_object,
                   _popup_comp_del_cb, NULL, inst);
             e_gadcon_popup_show(inst->popup);
             e_object_data_set(E_OBJECT(inst->popup), inst);
             E_OBJECT_DEL_SET(inst->popup, _popup_del_cb);
          }
     }
}

static E_Gadcon_Client *
_gc_init(E_Gadcon *gc, const char *name, const char *id, const char *style)
{
   Instance *inst;
   E_Gadcon_Client *gcc;
   char buf[4096];

   inst = _instance_create();

   snprintf(buf, sizeof(buf), "%s/kinjector.edj", e_module_dir_get(_module));

   inst->o_icon = edje_object_add(gc->evas);
   if (!e_theme_edje_object_set(inst->o_icon,
				"base/theme/modules/kinjector",
                                "modules/kinjector/main"))
      edje_object_file_set(inst->o_icon, buf, "modules/kinjector/main");
   evas_object_show(inst->o_icon);

   gcc = e_gadcon_client_new(gc, name, id, style, inst->o_icon);
   gcc->data = inst;
   inst->gcc = gcc;

   _config_dir_changed(inst, NULL, ECORE_FILE_EVENT_MODIFIED, NULL);
   evas_object_event_callback_add(inst->o_icon, EVAS_CALLBACK_MOUSE_DOWN,
				  _button_cb_mouse_down, inst);

   return gcc;
}

static void
_gc_shutdown(E_Gadcon_Client *gcc)
{
//   printf("TRANS: In - %s", __FUNCTION__);
   _instance_delete(gcc->data);
}

static void
_gc_orient(E_Gadcon_Client *gcc, E_Gadcon_Orient orient EINA_UNUSED)
{
   e_gadcon_client_aspect_set(gcc, 32, 16);
   e_gadcon_client_min_size_set(gcc, 32, 16);
}

static const char *
_gc_label(const E_Gadcon_Client_Class *client_class EINA_UNUSED)
{
   return "KInjector";
}

static Evas_Object *
_gc_icon(const E_Gadcon_Client_Class *client_class EINA_UNUSED, Evas *evas)
{
   Evas_Object *o;
   char buf[4096];

   if (!_module) return NULL;

   snprintf(buf, sizeof(buf), "%s/e-module-kinjector.edj", e_module_dir_get(_module));

   o = edje_object_add(evas);
   edje_object_file_set(o, buf, "icon");
   return o;
}

static const char *
_gc_id_new(const E_Gadcon_Client_Class *client_class)
{
   char buf[32];
   static int id = 0;
   sprintf(buf, "%s.%d", client_class->name, ++id);
   return eina_stringshare_add(buf);
}

EAPI E_Module_Api e_modapi =
{
   E_MODULE_API_VERSION, "KInjector"
};

static const E_Gadcon_Client_Class _gc_class =
{
   GADCON_CLIENT_CLASS_VERSION, "kinjector",
   {
      _gc_init, _gc_shutdown, _gc_orient, _gc_label, _gc_icon, _gc_id_new, NULL, NULL
   },
   E_GADCON_CLIENT_STYLE_PLAIN
};

EAPI void *
e_modapi_init(E_Module *m)
{
//   printf("TRANS: In - %s", __FUNCTION__);
   ecore_init();
   ecore_con_init();
   ecore_con_url_init();
   efreet_init();

   _module = m;
   e_gadcon_provider_register(&_gc_class);

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
//   printf("TRANS: In - %s", __FUNCTION__);
   e_gadcon_provider_unregister(&_gc_class);

   _module = NULL;
   efreet_shutdown();
   ecore_con_url_shutdown();
   ecore_con_shutdown();
   ecore_shutdown();
   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   //e_config_domain_save("module.kinjector", conf_edd, cpu_conf);
   return 1;
}
#else
int main(int argc, char **argv)
{
   Instance *inst;

   eina_init();
   ecore_init();
   ecore_con_init();
   efreet_init();
   elm_init(argc, argv);

   inst = _instance_create();
   if (!inst)
     {
        PRINT("Failed to initialize the module\n"
              "Did you add a rule in /etc/udev/rules.d/ to chmod 666 /dev/uinput?\n"
              "Did you add uinput to the modules to load (/etc/modules-load.d/?");
        goto end;
     }

   Eo *win = elm_win_add(NULL, "KInjector", ELM_WIN_BASIC);

   Eo *o = elm_box_add(win);
   evas_object_size_hint_align_set(o, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(o, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(o);
   elm_win_resize_object_add(win, o);
   efl_wref_add(o, &inst->main_box);

   evas_object_resize(win, 480, 480);
   evas_object_show(win);

   _config_dir_changed(inst, NULL, ECORE_FILE_EVENT_MODIFIED, NULL);

   elm_run();

   _instance_delete(inst);
end:
   elm_shutdown();
   ecore_con_shutdown();
   ecore_shutdown();
   eina_shutdown();
   return 0;
}
#endif
