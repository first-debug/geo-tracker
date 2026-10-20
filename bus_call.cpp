#include "glib.h"
#include "gst/gstbus.h"
#include "gst/gstutils.h"

#include "bus_call.hpp"

// Имя source-bin, которому принадлежит объект, или NULL. Освобождать через g_free.
static gchar *find_source_bin_name(GstObject *object) {
    GstObject *current = object ? GST_OBJECT(gst_object_ref(object)) : NULL;

    while (current != NULL) {
        gchar *name = gst_object_get_name(current);
        if (name != NULL && g_str_has_prefix(name, "source-bin-")) {
            gst_object_unref(current);
            return name;
        }
        g_free(name);

        GstObject *parent = gst_object_get_parent(current);
        gst_object_unref(current);
        current = parent;
    }
    return NULL;
}

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    BusCallData *data_struct = (BusCallData *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream.\n");
            g_main_loop_quit(data_struct->loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);

            gchar *src_name = gst_object_get_name(GST_MESSAGE_SRC(msg));
            gchar *source_bin = find_source_bin_name(GST_MESSAGE_SRC(msg));

            if (source_bin != NULL) {
                // Ошибка одной камеры не должна останавливать остальные:
                // nvstreammux продолжит отправлять пачки без неё.
                g_printerr("Error in %s (%s), camera is lost, other cameras keep working: %s\n%s\n",
                        source_bin, src_name, error->message, debug ? debug : "");
            } else {
                g_printerr("Error from %s: %s\n%s\n", src_name, error->message, debug ? debug : "");
                g_main_loop_quit(data_struct->loop);
            }

            g_free(source_bin);
            g_free(src_name);
            g_error_free(error);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_WARNING: {
            gchar *debug;
            GError *error;
            gst_message_parse_warning(msg, &error, &debug);

            gchar *src_name = gst_object_get_name(GST_MESSAGE_SRC(msg));
            g_printerr("Warning from %s: %s\n%s\n", src_name, error->message, debug ? debug : "");

            g_free(src_name);
            g_error_free(error);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC (msg) != GST_OBJECT (data_struct->pipeline))
                return TRUE;

            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

            g_print("%-21s state has been changed from %s to %s, pending state %s\n",
                    msg->src->name, gst_element_state_get_name(old_state), gst_element_state_get_name(new_state),
                    gst_element_state_get_name(pending_state));
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structer = gst_message_get_structure(msg);
            if (structer == NULL)
                return TRUE;
            const gchar *elem_name = gst_structure_get_name(structer);
            GstObject *src_object = GST_MESSAGE_SRC(msg);

            gchar *src_name = gst_object_get_name(src_object);
            gchar *struct_str = gst_structure_to_string(structer);

            g_print("Catched MESSAGE_ELEMENT from = %s\n", src_name);
            g_print("%s\n", struct_str);
            g_free(src_name);
            g_free(struct_str);
            break;
        }
        default: {
        }
    }
    return TRUE;
}

