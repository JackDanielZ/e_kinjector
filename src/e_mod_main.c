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

#ifndef STAND_ALONE
#include <e.h>
#else
#include <Elementary.h>
#endif
#include <Eina.h>
#include <Ecore.h>
#include <Ecore_Con.h>
#include "e_mod_main.h"

#define _EET_ENTRY "config"

typedef struct
{
   Eina_Stringshare *filename;
   Eo *obj;
} Item_Desc;

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
} Instance;

#ifndef STAND_ALONE
static E_Module *_module = NULL;
#endif

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

static Eo *
_button_create(Eo *parent, const char *text, Eo *icon, Eo **wref, Evas_Smart_Cb cb_func, void *cb_data)
{
   Eo *bt = wref ? *wref : NULL;
   if (!bt)
     {
        bt = elm_button_add(parent);
        evas_object_size_hint_align_set(bt, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(bt, EVAS_HINT_EXPAND, 0.0);
        evas_object_show(bt);
        if (wref) efl_wref_add(bt, wref);
        if (cb_func) evas_object_smart_callback_add(bt, "clicked", cb_func, cb_data);
     }
   elm_object_text_set(bt, text);
   elm_object_part_content_set(bt, "icon", icon);
   return bt;
}

static void
_start_pause_bt_clicked(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
}

static void
_file_tooltip_show(void *data EINA_UNUSED, Evas *e EINA_UNUSED, Evas_Object *obj, void *event_info EINA_UNUSED)
{
   elm_object_tooltip_show(obj);
}

static void
_file_tooltip_hide(void *data EINA_UNUSED, Evas *e EINA_UNUSED, Evas_Object *obj, void *event_info EINA_UNUSED)
{
   elm_object_tooltip_hide(obj);
}

static void
_tooltip_enable(Eo *obj, Eina_Bool enable)
{
   if (!obj) return;
   elm_object_disabled_set(obj, enable);
   if (enable)
     {
        evas_object_event_callback_add(obj, EVAS_CALLBACK_MOUSE_IN, _file_tooltip_show, NULL);
        evas_object_event_callback_add(obj, EVAS_CALLBACK_MOUSE_OUT, _file_tooltip_hide, NULL);
     }
   else
     {
        elm_object_tooltip_text_set(obj, NULL);
        elm_object_tooltip_hide(obj);
        evas_object_event_callback_del_full(obj, EVAS_CALLBACK_MOUSE_IN, _file_tooltip_show, NULL);
        evas_object_event_callback_del_full(obj, EVAS_CALLBACK_MOUSE_OUT, _file_tooltip_hide, NULL);
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
        _label_create(inst->main_box, idesc->filename, &idesc->obj);
        elm_box_pack_end(inst->main_box, idesc->obj);
     }
}

static void
_config_dir_changed(void *data,
      Ecore_File_Monitor *em EINA_UNUSED,
      Ecore_File_Event event, const char *path EINA_UNUSED)
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
                  if (!found && !strcmp(file, idesc->filename))
                    {
                       found = EINA_TRUE;
                       items = eina_list_remove_list(items, itr);
                       inst->items = eina_list_append(inst->items, idesc);
                    }
               }
             if (!found)
               {
                  idesc = calloc(1, sizeof(*idesc));
                  idesc->filename = eina_stringshare_add(file);
                  inst->items = eina_list_append(inst->items, idesc);
               }
          }
        free(file);
     }
   EINA_LIST_FREE(items, idesc)
     {
        eina_stringshare_del(idesc->filename);
        free(idesc);
     }
   _box_update(inst, EINA_TRUE);
}

static Eina_Bool
_mkdir(const char *dir)
{
   if (!ecore_file_exists(dir))
     {
        Eina_Bool success = ecore_file_mkdir(dir);
        if (!success)
          {
             printf("Cannot create a config folder \"%s\"\n", dir);
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
//   printf("TRANS: In - %s\n", __FUNCTION__);
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
//   printf("TRANS: In - %s\n", __FUNCTION__);
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
//   printf("TRANS: In - %s\n", __FUNCTION__);
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
   elm_shutdown();
   ecore_con_shutdown();
   ecore_shutdown();
   eina_shutdown();
   return 0;
}
#endif
