/*
   UFRaw - Unidentified Flying Raw
   Raw photo loader plugin for The GIMP
   by Udi Fuchs,

   based on the GIMP plug-in by Pawel T. Jochym jochym at ifj edu pl,

   based on the GIMP plug-in by Dave Coffin
   http://www.cybercom.net/~dcoffin/

   UFRaw is licensed under the GNU General Public License.
   It uses DCRaw code to do the actual raw decoding.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <gtk/gtk.h>
#include "ufraw.h"
#include "curveeditor_widget.h"
#include "ufraw_icon.h"

#ifdef HAVE_GTK_2_6
void ufraw_chooser_toggle(GtkToggleButton *button, GtkFileChooser *filechooser);
#endif

/* Set to huge number so that the preview size is set by the screen size */
const int def_preview_width = 9000;
const int def_preview_height = 9000;
#define raw_his_size 320
#define live_his_size 256
#define min_scale 2
#define max_scale 20

enum { pixel_format, percent_format };

char *expanderText[] = { "Raw histogram", "Exposure", "White balance",
    "Color management", "Curve corrections", "Live histogram", NULL };

typedef struct {
    GtkLabel *labels[4];
    int num;
    int format;
} colorLabels;

/* All the "global" information is here: */
typedef struct {
    ufraw_data *UF;
    conf_data SaveConfig;
    double initialChanMul[4];
    GList *WBPresets; /* List of WB presets in WBCombo*/
    /* Remember the Gtk Widgets that we need to access later */
    GtkWidget *ControlsBox, *PreviewWidget, *RawHisto, *LiveHisto;
    GtkWidget *BaseCurveWidget, *CurveWidget, *BlackLabel;
    GtkComboBox *WBCombo, *BaseCurveCombo, *CurveCombo,
		*ProfileCombo[profile_types];
    GtkLabel *SpotPatch;
    colorLabels *SpotLabels, *AvrLabels, *DevLabels, *OverLabels, *UnderLabels;
    GtkToggleButton *AutoExposureButton, *AutoBlackButton;
    GtkButton *AutoCurveButton;
    GtkWidget *ResetWBButton, *ResetGammaButton, *ResetLinearButton;
    GtkWidget *ResetExposureButton, *ResetSaturationButton;
    GtkWidget *ResetBlackButton, *ResetBaseCurveButton, *ResetCurveButton;
    GtkWidget *UseMatrixButton;
    GtkTooltips *ToolTips;
    GtkProgressBar *ProgressBar;
    /* We need the adjustments for update_scale() */
    GtkAdjustment *WBTuningAdjustment;
    GtkAdjustment *TemperatureAdjustment;
    GtkAdjustment *GreenAdjustment;
    GtkAdjustment *ChannelAdjustment[4];
    GtkAdjustment *GammaAdjustment;
    GtkAdjustment *LinearAdjustment;
    GtkAdjustment *ExposureAdjustment;
    GtkAdjustment *SaturationAdjustment;
    GtkAdjustment *ZoomAdjustment;
    long (*SaveFunc)();
    long RenderMode;
    /* Some actions update the progress bar while working, but meanwhile we
     * want to freeze all other actions. After we thaw the dialog we must
     * call update_scales() which was also frozen. */
    gboolean FreezeDialog;
    int SpotX1, SpotY1, SpotX2, SpotY2;
} preview_data;

/* These #defines are not very elegant, but otherwise things get tooo long */
#define CFG data->UF->conf
#define Developer data->UF->developer
#define CFG_cameraCurve (CFG->BaseCurve[camera_curve].m_numAnchors>0)

enum { base_curve, luminosity_curve };

preview_data *get_preview_data(void *object)
{
    GtkWidget *widget;
    if (GTK_IS_ADJUSTMENT(object)) {
	widget = g_object_get_data(object, "Parent-Widget");
    } else if (GTK_IS_MENU(object)) {
	widget = g_object_get_data(G_OBJECT(object), "Parent-Widget");
    } else if (GTK_IS_MENU_ITEM(object)) {
	GtkWidget *menu = gtk_widget_get_ancestor(object, GTK_TYPE_MENU);
	widget = g_object_get_data(G_OBJECT(menu), "Parent-Widget");
    } else {
	widget = object;
    }
    GtkWidget *parentWindow = gtk_widget_get_toplevel(widget);
    preview_data *data =
	    g_object_get_data(G_OBJECT(parentWindow), "Preview-Data");
    return data;
}

void ufraw_focus(void *window, gboolean focus)
{
    if (focus) {
        GtkWindow *parentWindow = (void *)ufraw_message(UFRAW_SET_PARENT, (char *)window);
	g_object_set_data(G_OBJECT(window), "WindowParent", parentWindow);
	if (parentWindow!=NULL)
	    gtk_window_set_accept_focus(GTK_WINDOW(parentWindow), FALSE);
    } else {
	GtkWindow *parentWindow = g_object_get_data(G_OBJECT(window),
		"WindowParent");
	ufraw_message(UFRAW_SET_PARENT, (char *)parentWindow);
	if (parentWindow!=NULL)
	    gtk_window_set_accept_focus(GTK_WINDOW(parentWindow), TRUE);
    }
}

void ufraw_messenger(char *message,  void *parentWindow)
{
    GtkDialog *dialog;

    if (parentWindow==NULL) {
	ufraw_batch_messenger(message);
    } else {
	dialog = GTK_DIALOG(gtk_message_dialog_new(GTK_WINDOW(parentWindow),
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, message));
	gtk_dialog_run(dialog);
	gtk_widget_destroy(GTK_WIDGET(dialog));
    }
}

void load_curve(GtkWidget *widget, long curveType)
{
    preview_data *data = get_preview_data(widget);
    GtkFileChooser *fileChooser;
    GtkFileFilter *filter;
    GSList *list, *saveList;
    CurveData c;
    char *cp;
    int curveCount;

    if (data->FreezeDialog) return;
    if (curveType==base_curve) curveCount = CFG->BaseCurveCount;
    else curveCount = CFG->curveCount;
    if (curveCount>=max_curves) {
        ufraw_message(UFRAW_ERROR, "No more room for new curves.");
        return;
    }
    fileChooser = GTK_FILE_CHOOSER(gtk_file_chooser_dialog_new("Load curve",
            GTK_WINDOW(gtk_widget_get_toplevel(widget)),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
            GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL));
    ufraw_focus(fileChooser, TRUE);
    gtk_file_chooser_set_select_multiple(fileChooser, TRUE);
#ifdef HAVE_GTK_2_6
    gtk_file_chooser_set_show_hidden(fileChooser, FALSE);
    GtkWidget *button = gtk_check_button_new_with_label( "Show hidden files");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    g_signal_connect(G_OBJECT(button), "toggled",
	    G_CALLBACK(ufraw_chooser_toggle), fileChooser);
    gtk_file_chooser_set_extra_widget(fileChooser, button);
#endif
    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "All curve formats");
    gtk_file_filter_add_pattern(filter, "*.ntc");
    gtk_file_filter_add_pattern(filter, "*.NTC");
    gtk_file_filter_add_pattern(filter, "*.ncv");
    gtk_file_filter_add_pattern(filter, "*.NCV");
    gtk_file_filter_add_pattern(filter, "*.curve");
    gtk_file_filter_add_pattern(filter, "*.CURVE");
    gtk_file_chooser_add_filter(fileChooser, filter);

    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "UFRaw curve format");
    gtk_file_filter_add_pattern(filter, "*.curve");
    gtk_file_filter_add_pattern(filter, "*.CURVE");
    gtk_file_chooser_add_filter(fileChooser, filter);

    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "Nikon curve format");
    gtk_file_filter_add_pattern(filter, "*.ntc");
    gtk_file_filter_add_pattern(filter, "*.NTC");
    gtk_file_filter_add_pattern(filter, "*.ncv");
    gtk_file_filter_add_pattern(filter, "*.NCV");
    gtk_file_chooser_add_filter(fileChooser, filter);

    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "All files");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(fileChooser, filter);

    if (strlen(CFG->curvePath)>0)
        gtk_file_chooser_set_current_folder(fileChooser, CFG->curvePath);
    if (gtk_dialog_run(GTK_DIALOG(fileChooser))==GTK_RESPONSE_ACCEPT) {
        for (list=saveList=gtk_file_chooser_get_filenames(fileChooser);
            list!=NULL && curveCount<max_curves;
            list=g_slist_next(list)) {
            c = conf_default.curve[0];
            if (curve_load(&c, list->data)!=UFRAW_SUCCESS) continue;
	    if (curveType==base_curve) {
		gtk_combo_box_append_text(data->BaseCurveCombo, c.name);
		CFG->BaseCurve[curveCount++] = c;
		CFG->BaseCurveIndex = curveCount-1;
		if (CFG_cameraCurve)
		    gtk_combo_box_set_active(data->BaseCurveCombo,
			    CFG->BaseCurveIndex);
		else
		    gtk_combo_box_set_active(data->BaseCurveCombo,
			    CFG->BaseCurveIndex-2);
	    } else {
		gtk_combo_box_append_text(data->CurveCombo, c.name);
		CFG->curve[curveCount++] = c;
		CFG->curveIndex = curveCount-1;
		gtk_combo_box_set_active(data->CurveCombo, CFG->curveIndex);
	    }
            cp = g_path_get_dirname(list->data);
            g_strlcpy(CFG->curvePath, cp, max_path);
            g_free(cp);
            g_free(list->data);
        }
        if (list!=NULL)
        ufraw_message(UFRAW_ERROR, "No more room for new curves.");
        g_slist_free(saveList);
    }
    if (curveType==base_curve) CFG->BaseCurveCount = curveCount;
    else CFG->curveCount = curveCount;
    ufraw_focus(fileChooser, FALSE);
    gtk_widget_destroy(GTK_WIDGET(fileChooser));
}

void save_curve(GtkWidget *widget, long curveType)
{
    preview_data *data = get_preview_data(widget);
    GtkFileChooser *fileChooser;
    GtkFileFilter *filter;
    char defFilename[max_name];
    char *filename, *cp;

    if (data->FreezeDialog) return;

    fileChooser = GTK_FILE_CHOOSER(gtk_file_chooser_dialog_new("Save curve",
            GTK_WINDOW(gtk_widget_get_toplevel(widget)),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
            GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL));
    ufraw_focus(fileChooser, TRUE);
#ifdef HAVE_GTK_2_6
    gtk_file_chooser_set_show_hidden(fileChooser, FALSE);
    GtkWidget *button = gtk_check_button_new_with_label( "Show hidden files");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    g_signal_connect(G_OBJECT(button), "toggled",
	    G_CALLBACK(ufraw_chooser_toggle), fileChooser);
    gtk_file_chooser_set_extra_widget(fileChooser, button);
#endif
    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "All curve formats");
    gtk_file_filter_add_pattern(filter, "*.ntc");
    gtk_file_filter_add_pattern(filter, "*.NTC");
    gtk_file_filter_add_pattern(filter, "*.ncv");
    gtk_file_filter_add_pattern(filter, "*.NCV");
    gtk_file_filter_add_pattern(filter, "*.curve");
    gtk_file_filter_add_pattern(filter, "*.CURVE");
    gtk_file_chooser_add_filter(fileChooser, filter);

    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "UFRaw curve format");
    gtk_file_filter_add_pattern(filter, "*.curve");
    gtk_file_filter_add_pattern(filter, "*.CURVE");
    gtk_file_chooser_add_filter(fileChooser, filter);

    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "Nikon curve format");
    gtk_file_filter_add_pattern(filter, "*.ntc");
    gtk_file_filter_add_pattern(filter, "*.NTC");
    gtk_file_filter_add_pattern(filter, "*.ncv");
    gtk_file_filter_add_pattern(filter, "*.NCV");
    gtk_file_chooser_add_filter(fileChooser, filter);

    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "All files");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(fileChooser, filter);

    if (strlen(CFG->curvePath)>0)
        gtk_file_chooser_set_current_folder(fileChooser, CFG->curvePath);
    if (curveType==base_curve)
	snprintf(defFilename, max_name, "%s.curve",
		CFG->BaseCurve[CFG->BaseCurveIndex].name);
    else
	snprintf(defFilename, max_name, "%s.curve",
		CFG->curve[CFG->curveIndex].name);
    gtk_file_chooser_set_current_name(fileChooser, defFilename);
    if (gtk_dialog_run(GTK_DIALOG(fileChooser))==GTK_RESPONSE_ACCEPT) {
        filename = gtk_file_chooser_get_filename(fileChooser);
	if (curveType==base_curve)
	    curve_save(&CFG->BaseCurve[CFG->BaseCurveIndex], filename);
	else
	    curve_save(&CFG->curve[CFG->curveIndex], filename);
        cp = g_path_get_dirname(filename);
        g_strlcpy(CFG->curvePath, cp, max_path);
        g_free(cp);
        g_free(filename);
    }
    ufraw_focus(fileChooser, FALSE);
    gtk_widget_destroy(GTK_WIDGET(fileChooser));
}

void load_profile(GtkWidget *widget, long type)
{
    preview_data *data = get_preview_data(widget);
    GtkFileChooser *fileChooser;
    GSList *list, *saveList;
    GtkFileFilter *filter;
    char *filename, *cp;
    profile_data p;

    if (data->FreezeDialog) return;
    if (CFG->profileCount[type]==max_profiles) {
        ufraw_message(UFRAW_ERROR, "No more room for new profiles.");
        return;
    }
    fileChooser = GTK_FILE_CHOOSER(gtk_file_chooser_dialog_new(
            "Load color profile",
            GTK_WINDOW(gtk_widget_get_toplevel(widget)),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
            GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL));
    ufraw_focus(fileChooser, TRUE);
    gtk_file_chooser_set_select_multiple(fileChooser, TRUE);
#ifdef HAVE_GTK_2_6
    gtk_file_chooser_set_show_hidden(fileChooser, FALSE);
    GtkWidget *button = gtk_check_button_new_with_label( "Show hidden files");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    g_signal_connect(G_OBJECT(button), "toggled",
	    G_CALLBACK(ufraw_chooser_toggle), fileChooser);
    gtk_file_chooser_set_extra_widget(fileChooser, button);
#endif
    if (strlen(CFG->profilePath)>0)
        gtk_file_chooser_set_current_folder(fileChooser, CFG->profilePath);
    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "Color Profiles");
    gtk_file_filter_add_pattern(filter, "*.icm");
    gtk_file_filter_add_pattern(filter, "*.ICM");
    gtk_file_filter_add_pattern(filter, "*.icc");
    gtk_file_filter_add_pattern(filter, "*.ICC");
    gtk_file_chooser_add_filter(fileChooser, filter);
    filter = GTK_FILE_FILTER(gtk_file_filter_new());
    gtk_file_filter_set_name(filter, "All files");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(fileChooser, filter);
    if (gtk_dialog_run(GTK_DIALOG(fileChooser))==GTK_RESPONSE_ACCEPT) {
        for (list=saveList=gtk_file_chooser_get_filenames(fileChooser);
            list!=NULL && CFG->profileCount[type]<max_profiles;
            list=g_slist_next(list))
        {
            filename = list->data;
            p = conf_default.profile[type][1];
            g_strlcpy(p.file, filename, max_path);
	    /* Make sure we update product name */
	    Developer->updateTransform = TRUE;
            developer_profile(Developer, type, &p);
            if (Developer->profile[type]==NULL) {
                g_free(list->data);
                continue;
            }
	    char *base = g_path_get_basename(filename);
	    char *name = uf_file_set_type(base, "");
	    char *utf8 = g_filename_to_utf8(name, -1, NULL, NULL, NULL);
	    if (utf8==NULL) utf8 = g_strdup("Unknown file name");
	    g_strlcpy(p.name, utf8, max_name);
	    g_free(utf8);
	    g_free(name);
	    g_free(base);
            /* Set some defaults to the profile's curve */
	    p.gamma = profile_default_gamma(&p);

            CFG->profile[type][CFG->profileCount[type]++] = p;
            gtk_combo_box_append_text(data->ProfileCombo[type], p.name);
            CFG->profileIndex[type] = CFG->profileCount[type]-1;
            cp = g_path_get_dirname(list->data);
            g_strlcpy(CFG->profilePath, cp, max_path);
            g_free(cp);
            g_free(list->data);
        }
        gtk_combo_box_set_active(data->ProfileCombo[type], 
		CFG->profileIndex[type]);
        if (list!=NULL)
            ufraw_message(UFRAW_ERROR, "No more room for new profiles.");
        g_slist_free(saveList);
    }
    ufraw_focus(fileChooser, FALSE);
    gtk_widget_destroy(GTK_WIDGET(fileChooser));
}

colorLabels *color_labels_new(GtkTable *table, int x, int y, char *label,
        int format)
{
    colorLabels *l;
    GtkWidget *lbl;
    int c, i = 0;

    l = g_new(colorLabels, 1);
    l->format = format;
    if (label!=NULL){
        lbl = gtk_label_new(label);
        gtk_misc_set_alignment(GTK_MISC(lbl), 1, 0.5);
        gtk_table_attach(table, lbl, x, x+1, y, y+1,
                GTK_SHRINK|GTK_FILL, GTK_FILL, 0, 0);
        i++;
    }
    for (c=0; c<3; c++, i++) {
        l->labels[c] = GTK_LABEL(gtk_label_new(NULL));
        gtk_table_attach_defaults(table, GTK_WIDGET(l->labels[c]),
                x+i, x+i+1, y, y+1);
    }
    return l;
}

void color_labels_set(colorLabels *l, double data[3])
{
    int c;
    char buf1[max_name], buf2[max_name];
    const char *colorName[] = {"red", "green", "blue"};

    for (c=0; c<3; c++) {
        switch (l->format) {
        case pixel_format: snprintf(buf1, max_name, "%3.f", data[c]); break;
        case percent_format:
                      if (data[c]<10.0)
                          snprintf(buf1, max_name, "%2.1f%%", data[c]);
                      else
                          snprintf(buf1, max_name, "%2.0f%%", data[c]);
                      break;
        default: snprintf(buf1, max_name, "ERR");
        }
        snprintf(buf2, max_name, "<span foreground='%s'>%s</span>",
                colorName[c], buf1);
        gtk_label_set_markup(l->labels[c], buf2);
    }
}

enum { render_default, render_overexposed, render_underexposed };

void render_preview(preview_data *data, long mode);
gboolean render_raw_histogram(preview_data *data);
gboolean render_preview_image(preview_data *data);
void render_spot(preview_data *data);
void draw_spot(preview_data *data, gboolean draw);

void render_preview_callback(GtkWidget *widget, long mode)
{
    preview_data *data = get_preview_data(widget);
    render_preview(data, mode);
}

static int renderRestart = FALSE;

void render_preview(preview_data *data, long mode)
{
    if (data->FreezeDialog) return;
    data->RenderMode = mode;
    g_idle_remove_by_data(data);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
            (GSourceFunc)(render_raw_histogram), data, NULL);
}

gboolean render_raw_histogram(preview_data *data)
{
    if (data->FreezeDialog) return FALSE;
    static GdkPixbuf *pixbuf;
    static guint8 *pixies;
    int rowstride;
    guint8 pix[99], *p8, pen[3], *p;
    guint16 pixtmp[9999];
    image_type p16;
    int x, c, cl, colors, y, y0, y1;
    int raw_his[raw_his_size][4], raw_his_max;

    if (CFG->autoExposure == apply_state) {
	ufraw_auto_expose(data->UF);
	gdouble lower, upper;
	g_object_get(data->ExposureAdjustment, "lower", &lower,
		"upper", &upper, NULL);
	/* Clip the exposure to prevent a "changed" signal */
	if (CFG->exposure > upper) CFG->exposure = upper;
	if (CFG->exposure < lower) CFG->exposure = lower;
	gtk_adjustment_set_value(data->ExposureAdjustment, CFG->exposure);
	gtk_widget_set_sensitive(data->ResetExposureButton,
		fabs( conf_default.exposure - CFG->exposure) > 0.001);
    }
    if (CFG->autoBlack == apply_state) {
	ufraw_auto_black(data->UF);
	curveeditor_widget_set_curve(data->CurveWidget,
		&CFG->curve[CFG->curveIndex]);
	gtk_widget_set_sensitive(data->ResetBlackButton,
		CFG->curve[CFG->curveIndex].m_anchors[0].x!=0.0 ||
		CFG->curve[CFG->curveIndex].m_anchors[0].y!=0.0 );
    }
    char text[max_name];
    g_snprintf(text, max_name, "Black point: %0.3lf",
	    CFG->curve[CFG->curveIndex].m_anchors[0].x);
    gtk_label_set_text(GTK_LABEL(data->BlackLabel), text);
    developer_prepare(Developer, data->UF->rgbMax, pow(2, CFG->exposure),
	    CFG->unclip, CFG->chanMul, data->UF->rgb_cam,
	    data->UF->colors, data->UF->useMatrix,
            &CFG->profile[0][CFG->profileIndex[0]],
            &CFG->profile[1][CFG->profileIndex[1]], CFG->intent,
            CFG->saturation, &CFG->BaseCurve[CFG->BaseCurveIndex],
            &CFG->curve[CFG->curveIndex]);

    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->RawHisto));
    if (gdk_pixbuf_get_height(pixbuf)!=CFG->rawHistogramHeight+2) {
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
	        raw_his_size+2, CFG->rawHistogramHeight+2);
	gtk_image_set_from_pixbuf(GTK_IMAGE(data->RawHisto), pixbuf);
	g_object_unref(pixbuf);
    }
    colors = data->UF->colors;
    pixies = gdk_pixbuf_get_pixels(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    memset(pixies, 0, (gdk_pixbuf_get_height(pixbuf)-1)*rowstride +
            gdk_pixbuf_get_width(pixbuf)*gdk_pixbuf_get_n_channels(pixbuf));
    memset(raw_his, 0, sizeof(raw_his));
    /* Collect histogram data */
    for (y=0; y<data->UF->image.height; y++)
        for (x=0; x<data->UF->image.width; x++)
            for (c=0; c<colors; c++)
                raw_his[MIN(
			data->UF->image.image[y*data->UF->image.width+x][c] *
                        (raw_his_size-1) / data->UF->rgbMax,
                        raw_his_size-1) ][c]++;
    for (x=0, raw_his_max=1; x<raw_his_size; x++) {
	for (c=0, y=0; c<colors; c++) {
	    if (CFG->rawHistogramScale==log_histogram)
		raw_his[x][c] = log(1+raw_his[x][c])*1000;
	    y += raw_his[x][c];
	}
        raw_his_max = MAX(raw_his_max, y);
    }
    p8 = pix;
    p = pixies + CFG->rawHistogramHeight*rowstride + 3;
    for (x=0; x<raw_his_size; x++) {
	/* draw the raw histogram */
        for (c=0, y0=0; c<colors; c++) {
	    for (cl=0; cl<colors; cl++) p16[cl] = 0;
	    p16[c] = data->UF->rgbMax;
            develope(pen, p16, Developer, 8, pixtmp, 1);
	    for (y=0; y<raw_his[x][c]*CFG->rawHistogramHeight/raw_his_max; y++)
		for (cl=0; cl<3; cl++)
		    pixies[(CFG->rawHistogramHeight-y-y0)*rowstride
			    + 3*(x+1)+cl] = pen[cl];
	    y0 += y;
	}
	/* draw curves on the raw histogram */
        for (c=0; c<colors; c++) {
	    for (cl=0; cl<colors; cl++) p16[cl] = 0;
	    p16[c] = data->UF->rgbMax;
            develope(pen, p16, Developer, 8, pixtmp, 1);
            /* Value for pixel x of color c in a grey pixel */
            for (cl=0; cl<colors; cl++)
                p16[cl] = MIN((guint64)x*Developer->rgbMax *
                          Developer->rgbWB[c] /
                          Developer->rgbWB[cl] / raw_his_size, 0xFFFF);
            develope(p8, p16, Developer, 8, pixtmp, 1);
            y=MAX(MAX(p8[0],p8[1]),p8[2]) * (CFG->rawHistogramHeight-1)/MAXOUT;
            /* Value for pixel x+1 of color c in a grey pixel */
            for (cl=0; cl<colors; cl++)
                p16[cl] = MIN((guint64)(x+1)*Developer->rgbMax *
                         Developer->rgbWB[c] /
                         Developer->rgbWB[cl]/raw_his_size-1, 0xFFFF);
            develope(p8, p16, Developer, 8, pixtmp, 1);
            y1=MAX(MAX(p8[0],p8[1]),p8[2]) * (CFG->rawHistogramHeight-1)/MAXOUT;
            for (; y<=y1; y++)
		for (cl=0; cl<3; cl++)
		    pixies[(CFG->rawHistogramHeight-y)*rowstride
			    + 3*(x+1)+cl] = pen[cl];
            /* Value for pixel x of pure color c */
            p16[0] = p16[1] = p16[2] = p16[3] = 0;
            p16[c] = MIN((guint64)x*Developer->rgbMax/raw_his_size, 0xFFFF);
            develope(p8, p16, Developer, 8, pixtmp, 1);
            y1=MAX(MAX(p8[0],p8[1]),p8[2]) * (CFG->rawHistogramHeight-1)/MAXOUT;
            for (; y<y1; y++)
		for (cl=0; cl<3; cl++)
		    pixies[(CFG->rawHistogramHeight-y)*rowstride
			    + 3*(x+1)+cl] = pen[cl]/2;
            /* Value for pixel x+1 of pure color c */
            p16[c] = MIN((guint64)(x+1)*Developer->rgbMax/raw_his_size - 1,
                     0xFFFF);
            develope(p8, p16, Developer, 8, pixtmp, 1);
            y1=MAX(MAX(p8[0],p8[1]),p8[2]) * (CFG->rawHistogramHeight-1)/MAXOUT;
            for (; y<=y1; y++)
		for (cl=0; cl<3; cl++)
		    pixies[(CFG->rawHistogramHeight-y)*rowstride
			    + 3*(x+1)+cl] = pen[cl];
        }
    }
    gtk_widget_queue_draw(data->RawHisto);
    renderRestart = TRUE;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
            (GSourceFunc)(render_preview_image), data, NULL);
    return FALSE;
}

gboolean render_preview_image(preview_data *data)
{
    if (data->FreezeDialog) return FALSE;
    static GdkPixbuf *pixbuf;
    static guint8 *pixies;
    static int width, height, x, y, c, o, min, max, rowstride, y0;
    static int live_his[live_his_size][4], live_his_max;
    guint8 *p8;
    guint16 pixtmp[9999];
    double rgb[3];
    guint64 sum[3], sqr[3];

    if (renderRestart) {
        renderRestart = FALSE;
        pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
        width = gdk_pixbuf_get_width(pixbuf);
        height = gdk_pixbuf_get_height(pixbuf);
        rowstride = gdk_pixbuf_get_rowstride(pixbuf);
        pixies = gdk_pixbuf_get_pixels(pixbuf);
#ifdef DEBUG
        fprintf(stderr, "render_preview_image: w=%d, h=%d, r=%d, mode=%d\n",
                width, height, rowstride, data->RenderMode);
        fflush(stderr);
#endif
        memset(live_his, 0, sizeof(live_his));
        y = y0 = 0;
    }
    for (; y<height; y++) {
        develope(&pixies[y*rowstride], data->UF->image.image[y*width],
                Developer, 8, pixtmp, width);
        for (x=0, p8=&pixies[y*rowstride]; x<width; x++, p8+=3) {
            for (c=0, max=0, min=0x100; c<3; c++) {
                max = MAX(max, p8[c]);
                min = MIN(min, p8[c]);
                live_his[p8[c]][c]++;
            }
            if (CFG->histogram==luminosity_histogram)
                live_his[(int)(0.3*p8[0]+0.59*p8[1]+0.11*p8[2])][3]++;
            if (CFG->histogram==value_histogram)
                live_his[max][3]++;
            if (CFG->histogram==saturation_histogram) {
                if (max==0) live_his[0][3]++;
                else live_his[255*(max-min)/max][3]++;
            }
            for (c=0; c<3; c++) {
                o = p8[c];
                if (data->RenderMode==render_default) {
                    if (CFG->overExp && max==MAXOUT) o = 0;
                    if (CFG->underExp && min==0) o = 255;
                } else if (data->RenderMode==render_overexposed) {
                    if (o!=255) o = 0;
                } else if (data->RenderMode==render_underexposed) {
                    if (o!=0) o = 255;
                }
                pixies[y*rowstride+3*x+c] = o;
            }
        }
        if (y%32==31) {
            gtk_widget_queue_draw_area(data->PreviewWidget,
		    0, y0, width, y+1-y0);
            y0 = y+1;
            y++;
            return TRUE;
        }
    }
    gtk_widget_queue_draw_area(data->PreviewWidget, 0, y0, width, y+1-y0);

    /* draw live histogram */
    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->LiveHisto));
    if (gdk_pixbuf_get_height(pixbuf)!=CFG->liveHistogramHeight+2) {
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
	        live_his_size+2, CFG->liveHistogramHeight+2);
	gtk_image_set_from_pixbuf(GTK_IMAGE(data->LiveHisto), pixbuf);
	g_object_unref(pixbuf);
    }
    pixies = gdk_pixbuf_get_pixels(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    memset(pixies, 0, (gdk_pixbuf_get_height(pixbuf)-1)*rowstride +
            gdk_pixbuf_get_width(pixbuf)*gdk_pixbuf_get_n_channels(pixbuf));
    for (c=0; c<3; c++) {
        sum[c] = 0;
        sqr[c] = 0;
        for (x=1; x<live_his_size; x++) {
            sum[c] += x*live_his[x][c];
            sqr[c] += (guint64)x*x*live_his[x][c];
        }
    }
    for (x=1, live_his_max=1; x<live_his_size-1; x++) {
	if (CFG->liveHistogramScale==log_histogram)
	    for (c=0; c<4; c++) live_his[x][c] = log(1+live_his[x][c])*1000;
        if (CFG->histogram==rgb_histogram)
            for (c=0; c<3; c++)
                live_his_max = MAX(live_his_max, live_his[x][c]);
        else if (CFG->histogram==r_g_b_histogram)
            live_his_max = MAX(live_his_max,
                    live_his[x][0]+live_his[x][1]+live_his[x][2]);
        else live_his_max = MAX(live_his_max, live_his[x][3]);
    }
#ifdef DEBUG
    fprintf(stderr, "render_preview: live_his_max=%d\n", live_his_max);
    fflush(stderr);
#endif
    for (x=0; x<live_his_size; x++) for (y=0; y<CFG->liveHistogramHeight; y++)
        if (CFG->histogram==r_g_b_histogram) {
            if (y*live_his_max < live_his[x][0]*CFG->liveHistogramHeight)
                pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+0] = 255;
            else if (y*live_his_max <
                    (live_his[x][0]+live_his[x][1])*CFG->liveHistogramHeight)
                pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+1] = 255;
            else if (y*live_his_max <
                    (live_his[x][0]+live_his[x][1]+live_his[x][2])
                    *CFG->liveHistogramHeight)
                pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+2] = 255;
        } else {
            for (c=0; c<3; c++)
                if (CFG->histogram==rgb_histogram) {
                    if (y*live_his_max<live_his[x][c]*CFG->liveHistogramHeight)
                        pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+c]
			    = 255;
                } else {
                    if (y*live_his_max<live_his[x][3]*CFG->liveHistogramHeight)
                        pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+c]
			    = 255;
                }
       }

    /* draw vertical line at quarters "behind" the live histogram */
    for (y=-1; y<CFG->liveHistogramHeight+1; y++) for (x=64; x<255; x+=64)
        if (pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+0]==0 &&
            pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+1]==0 &&
            pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+2]==0 )
            for (c=0; c<3; c++)
                pixies[(CFG->liveHistogramHeight-y)*rowstride+3*(x+1)+c]=64;
    gtk_widget_queue_draw(data->LiveHisto);
    for (c=0;c<3;c++) rgb[c] = sum[c]/height/width;
    color_labels_set(data->AvrLabels, rgb);
    for (c=0;c<3;c++) rgb[c] = sqrt(sqr[c]/height/width-rgb[c]*rgb[c]);
    color_labels_set(data->DevLabels, rgb);
    for (c=0;c<3;c++) rgb[c] = 100.0*live_his[live_his_size-1][c]/height/width;
    color_labels_set(data->OverLabels, rgb);
    for (c=0;c<3;c++) rgb[c] = 100.0*live_his[0][c]/height/width;
    color_labels_set(data->UnderLabels, rgb);
    render_spot(data);
    return FALSE;
}

void render_spot(preview_data *data)
{
    if (data->FreezeDialog) return;
    GdkPixbuf *pixbuf;
    guint8 *pixies;
    int width, height, rowstride;
    int c, y, x;
    int spotStartX, spotStartY, spotSizeX, spotSizeY;
    double rgb[3];
    char tmp[max_name];

    if (data->SpotX1<0) return;
    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    pixies = gdk_pixbuf_get_pixels(pixbuf);
    for (c=0; c<3; c++) rgb[c] = 0;
    /* Scale image coordinates to pixbuf coordinates */
    spotSizeY = abs(data->SpotY1 - data->SpotY2)
	    * height / data->UF->predictedHeight + 1;
    spotStartY = MIN(data->SpotY1, data->SpotY2)
	    *height / data->UF->predictedHeight;
    spotSizeX = abs(data->SpotX1 - data->SpotX2)
	    * width / data->UF->predictedWidth + 1;
    spotStartX = MIN(data->SpotX1, data->SpotX2)
	    * width / data->UF->predictedWidth;
    for (y=0; y<spotSizeY; y++) {
        for (x=0; x<spotSizeX; x++)
            for (c=0; c<3; c++)
		rgb[c] += pixies[(spotStartY+y)*rowstride+(spotStartX+x)*3+c];
    }
    for (c=0; c<3; c++) rgb[c] /= spotSizeX * spotSizeY;
    color_labels_set(data->SpotLabels, rgb);
    snprintf(tmp, max_name, "<span background='#%02X%02X%02X'>"
	    "                    </span>",
	    (int)rgb[0], (int)rgb[1], (int)rgb[2]);
    gtk_label_set_markup(data->SpotPatch, tmp);
    draw_spot(data, TRUE);
    gtk_widget_queue_draw_area(data->PreviewWidget, spotStartX-1, spotStartY-1,
	    spotStartX+spotSizeX+1, spotStartY+spotSizeY+1);
}

void pixbuf_mark(int x, int y, guchar *pixbuf, int width, int height,
	        int rowstride, gboolean draw)
{
    int c;

    if (x<0 || y<0 || x>=width || y>=height) return;
    for (c=0; c<3; c++) {
	if (draw) {
	    if ( (x+y)%2 == 0 )
		pixbuf[y*rowstride+3*x+c] = pixbuf[y*rowstride+3*x+c]/4;
	    else
		pixbuf[y*rowstride+3*x+c] = 255-(255-pixbuf[y*rowstride+3*x+c])/4;
	} else {
	    if ( (x+y)%2 == 0 )
		pixbuf[y*rowstride+3*x+c] = pixbuf[y*rowstride+3*x+c]*4;
	    else
		pixbuf[y*rowstride+3*x+c] = 255-(255-pixbuf[y*rowstride+3*x+c])*4;
	}
    }
}

void draw_spot(preview_data *data, gboolean draw)
{
    GdkPixbuf *pixbuf;
    guint8 *pixies;
    int width, height, x, y, rowstride;
    int spotStartX, spotStartY, spotSizeX, spotSizeY;

    if (data->SpotX1<0) return;
    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    pixies = gdk_pixbuf_get_pixels(pixbuf);
    /* Scale image coordinates to pixbuf coordinates */
    spotSizeY = abs(data->SpotY1 - data->SpotY2)
	    * height / data->UF->predictedHeight + 1;
    spotStartY = MIN(data->SpotY1, data->SpotY2)
	    *height / data->UF->predictedHeight;
    spotSizeX = abs(data->SpotX1 - data->SpotX2)
	    * width / data->UF->predictedWidth + 1;
    spotStartX = MIN(data->SpotX1, data->SpotX2)
	    * width / data->UF->predictedWidth;
    for (x=0; x<=spotSizeX; x++) {
        pixbuf_mark(spotStartX+x, spotStartY-1,
                pixies, width, height, rowstride, draw);
        pixbuf_mark(spotStartX+x, spotStartY+spotSizeY+1,
                pixies, width, height, rowstride, draw);
    }
    for (y=-1; y<=spotSizeY+1; y++) {
        pixbuf_mark(spotStartX-1, spotStartY+y,
                pixies, width, height, rowstride, draw);
        pixbuf_mark(spotStartX+spotSizeX+1, spotStartY+y,
	        pixies, width, height, rowstride, draw);
    }
    gtk_widget_queue_draw_area(data->PreviewWidget,
	    spotStartX-1, spotStartY-1,
	    spotSizeX+3, spotSizeY+3);
}

/* update the UI entries that could have changed automatically */
void update_scales(preview_data *data)
{
    if (data->FreezeDialog) return;
    data->FreezeDialog = TRUE;

    if (CFG->BaseCurveIndex>camera_curve && !CFG_cameraCurve)
        gtk_combo_box_set_active(data->BaseCurveCombo, CFG->BaseCurveIndex-2);
    else
        gtk_combo_box_set_active(data->BaseCurveCombo, CFG->BaseCurveIndex);
    gtk_combo_box_set_active(data->CurveCombo, CFG->curveIndex);

    int i;
    GList *l;
    gtk_combo_box_set_active(data->WBCombo, 0);
    for (i=0, l=data->WBPresets; l!=NULL; i++, l=g_list_next(l))
	if (!strcmp(CFG->wb, l->data))
	    gtk_combo_box_set_active(data->WBCombo, i);

    if (data->WBTuningAdjustment!=NULL)
	gtk_adjustment_set_value(data->WBTuningAdjustment, CFG->WBTuning);
    gtk_adjustment_set_value(data->TemperatureAdjustment, CFG->temperature);
    gtk_adjustment_set_value(data->GreenAdjustment, CFG->green);
    gtk_adjustment_set_value(data->ExposureAdjustment, CFG->exposure);
    gtk_adjustment_set_value(data->SaturationAdjustment, CFG->saturation);
    gtk_adjustment_set_value(data->GammaAdjustment,
            CFG->profile[0][CFG->profileIndex[0]].gamma);
    gtk_adjustment_set_value(data->LinearAdjustment,
            CFG->profile[0][CFG->profileIndex[0]].linear);
    for (i=0; i<data->UF->colors; i++)
	gtk_adjustment_set_value(data->ChannelAdjustment[i],
		CFG->chanMul[i]);
    gtk_adjustment_set_value(data->ZoomAdjustment, CFG->Zoom);

    if (GTK_WIDGET_SENSITIVE(data->UseMatrixButton)) {
	data->UF->useMatrix = CFG->profile[0][CFG->profileIndex[0]].useMatrix;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->UseMatrixButton),
		data->UF->useMatrix);
    }
    gtk_toggle_button_set_active(data->AutoExposureButton, CFG->autoExposure);
    gtk_toggle_button_set_active(data->AutoBlackButton, CFG->autoBlack);
    curveeditor_widget_set_curve(data->CurveWidget,
	    &CFG->curve[CFG->curveIndex]);

    gtk_widget_set_sensitive(data->ResetWBButton,
	    ( strcmp(CFG->wb, data->SaveConfig.wb)
	    ||fabs(CFG->temperature-data->SaveConfig.temperature)>1
	    ||fabs(CFG->green-data->SaveConfig.green)>0.001
	    ||CFG->chanMul[0]!=data->initialChanMul[0]
	    ||CFG->chanMul[1]!=data->initialChanMul[1]
	    ||CFG->chanMul[2]!=data->initialChanMul[2]
	    ||CFG->chanMul[3]!=data->initialChanMul[3] ) );
    gtk_widget_set_sensitive(data->ResetGammaButton,
	    fabs( profile_default_gamma(&CFG->profile[0][CFG->profileIndex[0]])
		- CFG->profile[0][CFG->profileIndex[0]].gamma) > 0.001);
    gtk_widget_set_sensitive(data->ResetLinearButton,
	    fabs( profile_default_linear(&CFG->profile[0][CFG->profileIndex[0]])
		- CFG->profile[0][CFG->profileIndex[0]].linear) > 0.001);
    gtk_widget_set_sensitive(data->ResetExposureButton,
	    fabs( conf_default.exposure - CFG->exposure) > 0.001);
    gtk_widget_set_sensitive(data->ResetSaturationButton,
	    fabs( conf_default.saturation - CFG->saturation) > 0.001);
    gtk_widget_set_sensitive(data->ResetBaseCurveButton,
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_numAnchors>2 ||
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[0].x!=0.0 ||
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[0].y!=0.0 ||
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[1].x!=1.0 ||
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[1].y!=1.0 );
    gtk_widget_set_sensitive(data->ResetBlackButton,
	    CFG->curve[CFG->curveIndex].m_anchors[0].x!=0.0 ||
	    CFG->curve[CFG->curveIndex].m_anchors[0].y!=0.0 );
    gtk_widget_set_sensitive(data->ResetCurveButton,
	    CFG->curve[CFG->curveIndex].m_numAnchors>2 ||
	    CFG->curve[CFG->curveIndex].m_anchors[1].x!=1.0 ||
	    CFG->curve[CFG->curveIndex].m_anchors[1].y!=1.0 );

    data->FreezeDialog = FALSE;
    render_preview(data, render_default);
};

void curve_update(GtkWidget *widget, long curveType)
{
    preview_data *data = get_preview_data(widget);
    if (curveType==base_curve) {
	CFG->BaseCurveIndex = manual_curve;
	CFG->BaseCurve[CFG->BaseCurveIndex] =
	    *curveeditor_widget_get_curve(data->BaseCurveWidget);
	if (CFG->autoExposure==enabled_state) CFG->autoExposure = apply_state;
	if (CFG->autoBlack==enabled_state) CFG->autoBlack = apply_state;
    } else {
	CFG->curveIndex = manual_curve;
	CFG->curve[CFG->curveIndex] =
	    *curveeditor_widget_get_curve(data->CurveWidget);
	CFG->autoBlack = FALSE;
    }
    update_scales(data);
}

void spot_wb_event(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    int spotStartX, spotStartY, spotSizeX, spotSizeY;
    int width, height, x, y, c;
    guint64 rgb[4];
    GdkPixbuf *pixbuf;

    user_data = user_data;

    if (data->FreezeDialog) return;
    if (data->SpotX1<=0) return;
    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    /* Scale image coordinates to pixbuf coordinates */
    spotSizeY = abs(data->SpotY1 - data->SpotY2)
	    * height / data->UF->predictedHeight + 1;
    spotStartY = MIN(data->SpotY1, data->SpotY2)
	    *height / data->UF->predictedHeight;
    spotSizeX = abs(data->SpotX1 - data->SpotX2)
	    * width / data->UF->predictedWidth + 1;
    spotStartX = MIN(data->SpotX1, data->SpotX2)
	    * width / data->UF->predictedWidth;

    for (c=0; c<4; c++) rgb[c] = 0;
    for (y=spotStartY; y<spotStartY+spotSizeY; y++)
        for (x=spotStartX; x<spotStartX+spotSizeX; x++)
            for (c=0; c<data->UF->colors; c++)
                rgb[c] += data->UF->image.image[y*data->UF->image.width+x][c];
    for (c=0; c<data->UF->colors; c++)
        CFG->chanMul[c] = (double)spotSizeX * spotSizeY * data->UF->rgbMax
		/ rgb[c];
    if (data->UF->colors<4) CFG->chanMul[3] = 0.0;
    ufraw_message(UFRAW_SET_LOG,
	    "spot_wb: channel multipliers = { %.0f, %.0f, %.0f, %.0f }\n",
            CFG->chanMul[0], CFG->chanMul[1], CFG->chanMul[2], CFG->chanMul[3]);
    g_strlcpy(CFG->wb, spot_wb, max_name);
    ufraw_set_wb(data->UF);
    if (CFG->autoExposure==enabled_state) CFG->autoExposure = apply_state;
    if (CFG->autoBlack==enabled_state) CFG->autoBlack = apply_state;
    update_scales(data);
}

void spot_press(GtkWidget *event_box, GdkEventButton *event, gpointer user_data)
{
    preview_data *data = get_preview_data(event_box);
    GdkPixbuf *pixbuf;
    int width, height;

    user_data = user_data;
    if (data->FreezeDialog) return;
    if (event->button!=1) return;
    draw_spot(data, FALSE);
    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    /* Scale pixbuf coordinates to image coordinates */
    data->SpotX1 = event->x * data->UF->predictedWidth / width;
    data->SpotY1 = event->y * data->UF->predictedHeight / height;
    data->SpotX2 = event->x * data->UF->predictedWidth / width;
    data->SpotY2 = event->y * data->UF->predictedHeight / height;
    render_spot(data);
}

gboolean spot_motion(GtkWidget *event_box, GdkEventMotion *event,
	gpointer user_data)
{
    preview_data *data = get_preview_data(event_box);
    GdkPixbuf *pixbuf;
    int width, height;

    user_data = user_data;
    if ((event->state&GDK_BUTTON1_MASK)==0) return FALSE;
    draw_spot(data, FALSE);
    pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    /* Scale pixbuf coordinates to image coordinates */
    data->SpotX2 = event->x * data->UF->predictedWidth / width;
    data->SpotY2 = event->y * data->UF->predictedHeight / height;
    render_spot(data);
    return FALSE;
}

gboolean create_base_image(preview_data *data)
{
    int shrinkSave = CFG->shrink;
    int sizeSave = CFG->size;
    if (CFG->Scale==0) {
	CFG->size = CFG->Zoom / 100.0 *
	    MAX(data->UF->predictedHeight, data->UF->predictedWidth);
	CFG->shrink = 0;
    } else {
	CFG->size = 0;
	CFG->shrink = CFG->Scale;
    }
    g_free(data->UF->image.image);
    data->UF->image.image = NULL;
    ufraw_convert_image(data->UF);
    CFG->shrink = shrinkSave;
    CFG->size = sizeSave;
    GdkPixbuf *pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(data->PreviewWidget));
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    if (width!=data->UF->image.width || height!=data->UF->image.height) {
	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
		data->UF->image.width, data->UF->image.height);
	gtk_image_set_from_pixbuf(GTK_IMAGE(data->PreviewWidget), pixbuf);
	g_object_unref(pixbuf);
    }
    char progressText[max_name];
    if (CFG->Scale==0)
	snprintf(progressText, max_name, "size %dx%d, zoom %2.f%%",
		data->UF->predictedHeight, data->UF->predictedWidth,
		CFG->Zoom);
    else
	snprintf(progressText, max_name, "size %dx%d, scale 1/%d",
		data->UF->predictedHeight, data->UF->predictedWidth,
		CFG->Scale);
    if (data->ProgressBar!=NULL) {
	gtk_progress_bar_set_text(data->ProgressBar, progressText);
	gtk_progress_bar_set_fraction(data->ProgressBar, 0);
    }
    return FALSE;
}

void zoom_in_event(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    user_data = user_data;
    if (data->FreezeDialog) return;
    if (CFG->Scale==0) {
	if (CFG->Zoom<100.0/max_scale) CFG->Zoom = 100.0/(max_scale+1);
	CFG->Scale = floor(100.0/CFG->Zoom);
	if (CFG->Scale==100.0/CFG->Zoom) CFG->Scale--;
    } else {
	CFG->Scale--;
    }
    if (CFG->Scale<min_scale) CFG->Scale = min_scale;
    if (CFG->Scale>max_scale) CFG->Scale = max_scale;
    CFG->Zoom = 100.0/CFG->Scale;
    create_base_image(data);
    gtk_adjustment_set_value(data->ZoomAdjustment, CFG->Zoom);
    render_preview(data, render_default);
}

void zoom_out_event(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    user_data = user_data;
    if (data->FreezeDialog) return;
    if (CFG->Scale==0) {
	if (CFG->Zoom<100.0/max_scale) CFG->Zoom = 100.0/(max_scale+1);
	CFG->Scale = ceil(100.0/CFG->Zoom);
	if (CFG->Scale==100.0/CFG->Zoom) CFG->Scale++;
    } else {
	CFG->Scale++;
    }
    if (CFG->Scale<min_scale) CFG->Scale = min_scale;
    if (CFG->Scale>max_scale) CFG->Scale = max_scale;
    CFG->Zoom = 100.0/CFG->Scale;
    create_base_image(data);
    gtk_adjustment_set_value(data->ZoomAdjustment, CFG->Zoom);
    render_preview(data, render_default);
}

GtkWidget *table_with_frame(GtkWidget *box, char *label, gboolean expand)
{
    GtkWidget *frame, *expander;
    GtkWidget *table;

    if (GTK_IS_NOTEBOOK(box)) {
	table = gtk_vbox_new(FALSE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(box),
		GTK_WIDGET(table), gtk_label_new(label));
    } else if (label!=NULL) {
	table = gtk_table_new(10, 10, FALSE);
        expander = gtk_expander_new(label);
        gtk_expander_set_expanded(GTK_EXPANDER(expander), expand);
        gtk_box_pack_start(GTK_BOX(box), expander, FALSE, FALSE, 0);
        frame = gtk_frame_new(NULL);
        gtk_container_add(GTK_CONTAINER(expander), GTK_WIDGET(frame));
        gtk_container_add(GTK_CONTAINER(frame), GTK_WIDGET(table));
    } else {
	table = gtk_table_new(10, 10, FALSE);
        frame = gtk_frame_new(NULL);
        gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(frame), GTK_WIDGET(table));
    }
    return table;
}

void button_update(GtkWidget *button, gpointer user_data)
{
    preview_data *data = get_preview_data(button);
    user_data = user_data;

    if (button==data->ResetWBButton) {
	int c;
	g_strlcpy(CFG->wb, data->SaveConfig.wb, max_name); 
	CFG->temperature = data->SaveConfig.temperature;
	CFG->green = data->SaveConfig.green;
	for (c=0; c<4; c++) CFG->chanMul[c] = data->initialChanMul[c];
    }
    if (button==data->ResetGammaButton) {
	CFG->profile[0][CFG->profileIndex[0]].gamma =
		profile_default_gamma(&CFG->profile[0][CFG->profileIndex[0]]);
    }
    if (button==data->ResetLinearButton) {
	CFG->profile[0][CFG->profileIndex[0]].linear =
		profile_default_linear(&CFG->profile[0][CFG->profileIndex[0]]);
    }
    if (button==GTK_WIDGET(data->AutoExposureButton)) {
	CFG->autoExposure = 
	    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    }
    if (button==data->ResetExposureButton) {
	CFG->exposure = conf_default.exposure;
	CFG->autoExposure = FALSE;
    }
    if (button==data->ResetSaturationButton) {
	CFG->saturation = conf_default.saturation;
    }
    if (button==GTK_WIDGET(data->AutoBlackButton)) {
	CFG->autoBlack = 
	    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    }
    if (button==data->ResetBlackButton) {
	CurveDataSetPoint(&CFG->curve[CFG->curveIndex], 0,
		conf_default.black, 0);
	CFG->autoBlack = FALSE;
    }
    if (button==GTK_WIDGET(data->AutoCurveButton)) {
	CFG->curveIndex = manual_curve;
	ufraw_auto_curve(data->UF);
    }
    if (button==data->ResetBaseCurveButton) {
	if (CFG->BaseCurveIndex==manual_curve) {
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_numAnchors = 2;
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[0].x = 0.0;
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[0].y = 0.0;
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[1].x = 1.0;
	    CFG->BaseCurve[CFG->BaseCurveIndex].m_anchors[1].y = 1.0;
	} else {
	    CFG->BaseCurveIndex = linear_curve;
	}
	curveeditor_widget_set_curve(data->BaseCurveWidget,
		&CFG->BaseCurve[CFG->BaseCurveIndex]);
    }
    if (button==data->ResetCurveButton) {
	if (CFG->curveIndex==manual_curve) {
	    CFG->curve[CFG->curveIndex].m_numAnchors = 2;
	    CFG->curve[CFG->curveIndex].m_anchors[1].x = 1.0;
	    CFG->curve[CFG->curveIndex].m_anchors[1].y = 1.0;
	} else {
	    CFG->curveIndex = linear_curve;
	}
    }
    if (CFG->autoExposure==enabled_state) CFG->autoExposure = apply_state;
    if (CFG->autoBlack==enabled_state) CFG->autoBlack = apply_state;
    update_scales(data);
    return;
}

void toggle_button_update(GtkToggleButton *button, gboolean *valuep)
{
    preview_data *data = get_preview_data(button);
    if (valuep==&CFG->unclip) {
	/* ugly flip needed because CFG->unclip==!clip_button_active */
	*valuep = !gtk_toggle_button_get_active(button);
	if (CFG->autoExposure==enabled_state) CFG->autoExposure = apply_state;
	char text[max_name];
        snprintf(text, max_name, "Highlight clipping\n"
		"Current state: %s", CFG->unclip ? "unclip" : "clip");
	gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(button), text, NULL);
	update_scales(data);
    } else {
	*valuep = gtk_toggle_button_get_active(button);
	if (valuep==&data->UF->useMatrix) {
	    CFG->profile[0][CFG->profileIndex[0]].useMatrix = *valuep;
	    if (CFG->autoExposure==enabled_state)
		CFG->autoExposure = apply_state;
	}
	render_preview(data, render_default);
    }
}

void toggle_button(GtkTable *table, int x, int y, char *label,
	gboolean *valuep)
{
    GtkWidget *widget, *align;

    widget = gtk_check_button_new_with_label(label);
    if (label==NULL) {
        align = gtk_alignment_new(1, 0, 0, 1);
        gtk_container_add(GTK_CONTAINER(align), widget);
    } else align = widget;
    gtk_table_attach(table, align, x, x+1, y, y+1, 0, 0, 0, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), *valuep);
    g_signal_connect(G_OBJECT(widget), "toggled",
            G_CALLBACK(toggle_button_update), valuep);
}

void adjustment_update(GtkAdjustment *adj, double *valuep)
{
    preview_data *data = get_preview_data(adj);
    if (data->FreezeDialog) return;
    if (valuep==&CFG->profile[0][0].gamma)
        valuep = (void *)&CFG->profile[0][CFG->profileIndex[0]].gamma;
    if (valuep==&CFG->profile[0][0].linear)
        valuep = (void *)&CFG->profile[0][CFG->profileIndex[0]].linear;

    /* Do noting if value didn't really change */
    long accuracy =
	(long)g_object_get_data(G_OBJECT(adj), "Adjustment-Accuracy");
    if ( fabs(*valuep-gtk_adjustment_get_value(adj))<pow(10,-accuracy)/2)
	return;
    *valuep = gtk_adjustment_get_value(adj);

    if (valuep==&CFG->temperature || valuep==&CFG->green) {
	g_strlcpy(CFG->wb, manual_wb, max_name); 
	ufraw_set_wb(data->UF);
    }
    if (valuep>=&CFG->chanMul[0] && valuep<=&CFG->chanMul[3]) {
	g_strlcpy(CFG->wb, spot_wb, max_name); 
	ufraw_set_wb(data->UF);
    }
    if (valuep==&CFG->WBTuning) {
	if ( strcmp(data->UF->conf->wb, auto_wb)==0 ||
	     strcmp(data->UF->conf->wb, camera_wb)==0 ) {
	    /* Prevent recalculation of Camera/Auto WB on WBTuning events */
	    data->UF->conf->WBTuning = 0;
	} else {
	    ufraw_set_wb(data->UF);
	}
    }
    if (valuep==&CFG->exposure) {
	CFG->autoExposure = FALSE;
    }
    if (valuep==&CFG->Zoom) {
	CFG->Scale = 0;
	create_base_image(data);
    } else {
	if (CFG->autoExposure==enabled_state) CFG->autoExposure = apply_state;
	if (CFG->autoBlack==enabled_state) CFG->autoBlack = apply_state;
    }
    update_scales(data);
}

GtkAdjustment *adjustment_scale(GtkTable *table,
    int x, int y, char *label, double value, double *valuep,
    double min, double max, double step, double jump, long accuracy, char *tip)
{
    preview_data *data = get_preview_data(table);
    GtkAdjustment *adj;
    GtkWidget *w, *l;

    w = gtk_event_box_new();
    l = gtk_label_new(label);
    gtk_misc_set_alignment(GTK_MISC(l), 1, 0.5);
    gtk_container_add(GTK_CONTAINER(w),l);
    gtk_tooltips_set_tip(data->ToolTips, w, tip, NULL);
    gtk_table_attach(table, w, x, x+1, y, y+1, GTK_SHRINK|GTK_FILL, 0, 0, 0);
    adj = GTK_ADJUSTMENT(gtk_adjustment_new(value, min, max, step, jump, 0));
    g_object_set_data(G_OBJECT(adj), "Adjustment-Accuracy",(gpointer)accuracy);

    w = gtk_hscale_new(adj);
    g_object_set_data(G_OBJECT(adj), "Parent-Widget", w);
    gtk_scale_set_draw_value(GTK_SCALE(w), FALSE);
    gtk_tooltips_set_tip(data->ToolTips, w, tip, NULL);
    gtk_table_attach(table, w, x+1, x+5, y, y+1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
    g_signal_connect(G_OBJECT(adj), "value-changed",
            G_CALLBACK(adjustment_update), valuep);

    w = gtk_spin_button_new(adj, step, accuracy);
    gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(w), FALSE);
    gtk_spin_button_set_update_policy(GTK_SPIN_BUTTON(w), GTK_UPDATE_IF_VALID);
    gtk_tooltips_set_tip(data->ToolTips, w, tip, NULL);
    gtk_table_attach(table, w, x+5, x+7, y, y+1, GTK_SHRINK|GTK_FILL, 0, 0, 0);
    return adj;
}

void combo_update(GtkWidget *combo, gint *valuep)
{
    preview_data *data = get_preview_data(combo);
    if (data->FreezeDialog) return;
    if ((char *)valuep==CFG->wb) {
	int i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
	g_strlcpy(CFG->wb, g_list_nth_data(data->WBPresets, i), max_name);
    } else {
	*valuep = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    }
    if (valuep==&CFG->BaseCurveIndex) {
        if (!CFG_cameraCurve && CFG->BaseCurveIndex>camera_curve-2)
            CFG->BaseCurveIndex += 2;
	curveeditor_widget_set_curve(data->BaseCurveWidget,
		&CFG->BaseCurve[CFG->BaseCurveIndex]);
    } else if (valuep==&CFG->curveIndex) {
	curveeditor_widget_set_curve(data->CurveWidget,
		&CFG->curve[CFG->curveIndex]);
    } else if ((char *)valuep==CFG->wb) {
        ufraw_set_wb(data->UF);
    }
    if (valuep!=&CFG->interpolation) {
	if (CFG->autoExposure==enabled_state) CFG->autoExposure = apply_state;
	if (CFG->autoBlack==enabled_state) CFG->autoBlack = apply_state;
        update_scales(data);
    }
}

void radio_menu_update(GtkWidget *item, gint *valuep)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item))) {
	preview_data *data = get_preview_data(item);
	*valuep = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),
		    "Radio-Value"));
	render_preview(data, render_default);
    }
}

void container_remove(GtkWidget *widget, gpointer user_data)
{
    GtkContainer *container = GTK_CONTAINER(user_data);
    gtk_container_remove(container, widget);
}

void delete_from_list(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    GtkDialog *dialog;
    long type, index;

    dialog = GTK_DIALOG(gtk_widget_get_ancestor(widget, GTK_TYPE_DIALOG));
    type = (long)g_object_get_data(G_OBJECT(widget), "Type");
    index = (long)user_data;
    if (type<2) {
        gtk_combo_box_remove_text(data->ProfileCombo[type], index);
        CFG->profileCount[type]--;
        if (CFG->profileIndex[type]==CFG->profileCount[type]) {
            CFG->profileIndex[type]--;
	}
	gtk_combo_box_set_active(data->ProfileCombo[type], 
		CFG->profileIndex[type]);
        for (; index<CFG->profileCount[type]; index++)
            CFG->profile[type][index] = CFG->profile[type][index+1];
    } else if (type==2+base_curve) {
        if (CFG_cameraCurve)
            gtk_combo_box_remove_text(data->BaseCurveCombo, index);
        else
            gtk_combo_box_remove_text(data->BaseCurveCombo, index-2);
        CFG->BaseCurveCount--;
        if (CFG->BaseCurveIndex==CFG->BaseCurveCount) {
            CFG->BaseCurveIndex--;
	    if (CFG->BaseCurveIndex==camera_curve && !CFG_cameraCurve)
		CFG->BaseCurveIndex = linear_curve;
	    if (CFG->BaseCurveIndex>camera_curve && !CFG_cameraCurve)
		gtk_combo_box_set_active(data->BaseCurveCombo,
			CFG->BaseCurveIndex-2);
	    else
		gtk_combo_box_set_active(data->BaseCurveCombo,
			CFG->BaseCurveIndex);
	    curveeditor_widget_set_curve(data->BaseCurveWidget,
		&CFG->BaseCurve[CFG->BaseCurveIndex]);
	}
        for (; index<CFG->BaseCurveCount; index++)
            CFG->BaseCurve[index] = CFG->BaseCurve[index+1];
    } else if (type==2+luminosity_curve) {
        gtk_combo_box_remove_text(data->CurveCombo, index);
        CFG->curveCount--;
        if (CFG->curveIndex==CFG->curveCount) {
            CFG->curveIndex--;
	    gtk_combo_box_set_active(data->CurveCombo, CFG->curveIndex);
	    curveeditor_widget_set_curve(data->CurveWidget,
		&CFG->curve[CFG->curveIndex]);
	}
        for (; index<CFG->curveCount; index++)
            CFG->curve[index] = CFG->curve[index+1];
    }
    gtk_dialog_response(dialog, GTK_RESPONSE_APPLY);
}

void configuration_save(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    user_data = user_data;
    conf_save(CFG, NULL, NULL);
    data->SaveConfig = *data->UF->conf;
}

void options_combo_update(GtkWidget *combo, gint *valuep)
{
    GtkDialog *dialog;

    *valuep = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    dialog = GTK_DIALOG(gtk_widget_get_ancestor(combo, GTK_TYPE_DIALOG));
    gtk_dialog_response(dialog, GTK_RESPONSE_APPLY);
}

void options_dialog(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    GtkWidget *optionsDialog, *profileTable[2];
    GtkComboBox *confCombo;
    GtkWidget *notebook, *label, *page, *button, *text, *box, *image, *event;
    GtkTable *baseCurveTable, *curveTable, *table;
    GtkTextBuffer *confBuffer, *buffer;
    char txt[max_name], *buf;
    long i, j;

    user_data = user_data;
    if (data->FreezeDialog) return;
    optionsDialog = gtk_dialog_new_with_buttons("UFRaw options",
            GTK_WINDOW(gtk_widget_get_toplevel(widget)),
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    g_object_set_data(G_OBJECT(optionsDialog), "Preview-Data", data);
    ufraw_focus(optionsDialog, TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(optionsDialog),
            GTK_RESPONSE_OK);
    notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(GTK_DIALOG(optionsDialog)->vbox),
            notebook);

    label =gtk_label_new("Settings");
    box = gtk_vbox_new(FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), box, label);
    profileTable[0] = table_with_frame(box, "Input color profiles", TRUE);
    profileTable[1] = table_with_frame(box, "Output color profiles", TRUE);
    baseCurveTable = GTK_TABLE(table_with_frame(box, "Base Curves", TRUE));
    curveTable = GTK_TABLE(table_with_frame(box, "Luminosity Curves", TRUE));

    label = gtk_label_new("Configuration");
    box = gtk_vbox_new(FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), box, label);

    page = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(box), page, TRUE, TRUE, 0);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    text = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(page), text);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    confBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));

    table = GTK_TABLE(gtk_table_new(10, 10, FALSE));
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(table), FALSE, FALSE, 0);
    label = gtk_label_new("Save image defaults ");
    event = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(event), label);
    gtk_tooltips_set_tip(data->ToolTips, event,
	    "Save current image manipulation parameters as defaults.\n"
	    "The output parameters in this window are always saved.", NULL);
    gtk_table_attach(GTK_TABLE(table), event, 0, 2, 0, 1, 0, 0, 0, 0);
    confCombo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    gtk_combo_box_append_text(confCombo, "Never again");
    gtk_combo_box_append_text(confCombo, "Always");
    gtk_combo_box_append_text(confCombo, "Just this once");
    gtk_combo_box_set_active(confCombo, CFG->saveConfiguration);
    g_signal_connect(G_OBJECT(confCombo), "changed",
	    G_CALLBACK(options_combo_update), &CFG->saveConfiguration);
    gtk_table_attach(GTK_TABLE(table), GTK_WIDGET(confCombo), 2, 3, 0, 1,
	    0, 0, 0, 0);
					    
    button = gtk_button_new_from_stock(GTK_STOCK_SAVE);
    gtk_tooltips_set_tip(data->ToolTips, button,
	    "Save resource file ($HOME/.ufrawrc) now", NULL);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(configuration_save), NULL);
    gtk_table_attach(table, button, 0, 1, 1, 2, 0, 0, 0, 0);

    label = gtk_label_new("Log");
    page = gtk_scrolled_window_new(NULL, NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, label);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    text = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(page), text);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    char *log = ufraw_message(UFRAW_GET_LOG, NULL);
    if (log!=NULL) {
	char *utf8_log = g_filename_to_utf8(log, -1, NULL, NULL, NULL);
	if (utf8_log==NULL) utf8_log = g_strdup("Encoding conversion failed");
	gtk_text_buffer_set_text(buffer, utf8_log, -1);
	g_free(utf8_log);
    }
    label = gtk_label_new("About");
    box = gtk_vbox_new(FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), box, label);
    image = gtk_image_new_from_pixbuf(
            gdk_pixbuf_new_from_inline(-1, ufraw_icon, FALSE, NULL));
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label),
            "<span size='xx-large'><b>UFRaw " VERSION "</b></span>");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), "\n"
            "The <b>U</b>nidentified <b>F</b>lying <b>Raw</b> "
            "(<b>UFRaw</b>) is a utility to read\n"
            "and manipulate raw images from digital cameras.\n"
	    "UFRaw relies on <b>D</b>igital <b>C</b>amera "
	    "<b>Raw</b> (<b>DCRaw</b>)\n"
	    "for the actual encoding of the raw images.\n\n"
            "Author: Udi Fuchs\n"
            "Homepage: http://ufraw.sourceforge.net/\n\n");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    while (1) {
        for (j=0; j<2; j++) {
            gtk_container_foreach(GTK_CONTAINER(profileTable[j]),
                    (GtkCallback)(container_remove), profileTable[j]);
            table = GTK_TABLE(profileTable[j]);
            for (i=1; i<CFG->profileCount[j]; i++) {
                snprintf(txt, max_name, "%s (%s)",
                        CFG->profile[j][i].name,
                        CFG->profile[j][i].productName);
                label = gtk_label_new(txt);
                gtk_table_attach_defaults(table, label, 0, 1, i, i+1);
                button = gtk_button_new_from_stock(GTK_STOCK_DELETE);
                g_object_set_data(G_OBJECT(button), "Type", (void*)j);
                g_signal_connect(G_OBJECT(button), "clicked",
                        G_CALLBACK(delete_from_list), (gpointer)i);
                gtk_table_attach(table, button, 1, 2, i, i+1, 0, 0, 0, 0);
            }
        }
        gtk_container_foreach(GTK_CONTAINER(baseCurveTable),
                (GtkCallback)(container_remove), baseCurveTable);
        table = baseCurveTable;
        for (i=camera_curve+1; i<CFG->BaseCurveCount; i++) {
            label = gtk_label_new(CFG->BaseCurve[i].name);
            gtk_table_attach_defaults(table, label, 0, 1, i, i+1);
            button = gtk_button_new_from_stock(GTK_STOCK_DELETE);
            g_object_set_data(G_OBJECT(button), "Type",
		    (gpointer)2+base_curve);
            g_signal_connect(G_OBJECT(button), "clicked",
                    G_CALLBACK(delete_from_list), (gpointer)i);
            gtk_table_attach(table, button, 1, 2, i, i+1, 0, 0, 0, 0);
        }
        gtk_container_foreach(GTK_CONTAINER(curveTable),
                (GtkCallback)(container_remove), curveTable);
        table = curveTable;
        for (i=linear_curve+1; i<CFG->curveCount; i++) {
            label = gtk_label_new(CFG->curve[i].name);
            gtk_table_attach_defaults(table, label, 0, 1, i, i+1);
            button = gtk_button_new_from_stock(GTK_STOCK_DELETE);
            g_object_set_data(G_OBJECT(button), "Type",
		    (gpointer)2+luminosity_curve);
            g_signal_connect(G_OBJECT(button), "clicked",
                    G_CALLBACK(delete_from_list), (gpointer)i);
            gtk_table_attach(table, button, 1, 2, i, i+1, 0, 0, 0, 0);
        }
        conf_save(CFG, NULL, &buf);
        gtk_text_buffer_set_text(confBuffer, buf, -1);
	g_free(buf);
        gtk_widget_show_all(optionsDialog);
        if (gtk_dialog_run(GTK_DIALOG(optionsDialog))!=
                    GTK_RESPONSE_APPLY) {
	    ufraw_focus(optionsDialog, FALSE);
            gtk_widget_destroy(optionsDialog);
	    render_preview(data, render_default);
            return;
        }
    }
}

gboolean window_delete_event(GtkWidget *widget, GdkEvent *event,
	gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    user_data = user_data;
    event = event;
    if (data->FreezeDialog) return TRUE;
    g_object_set_data(G_OBJECT(widget), "WindowResponse",
            (gpointer)GTK_RESPONSE_CANCEL);
    gtk_main_quit();
    return TRUE;
}

void expander_state(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    const char *text;
    GtkWidget *label;
    int i;

    user_data = user_data;
    if (!GTK_IS_EXPANDER(widget)) return;
    label = gtk_expander_get_label_widget(GTK_EXPANDER(widget));
    text = gtk_label_get_text(GTK_LABEL(label));
    for (i=0; expanderText[i]!=NULL; i++)
        if (!strcmp(text, expanderText[i]))
            CFG->expander[i] = gtk_expander_get_expanded(GTK_EXPANDER(widget));
}

gboolean histogram_menu(GtkMenu *menu, GdkEventButton *event)
{
    preview_data *data = get_preview_data(menu);
    if (data->FreezeDialog) return FALSE;
    if (event->button!=3) return FALSE;
    gtk_menu_popup(menu, NULL, NULL, NULL, NULL, event->button, event->time);
    return TRUE;
}

gboolean preview_cursor(GtkWidget *widget, GdkEventCrossing *event,
	GdkCursor *cursor)
{
    widget = widget;
    gdk_window_set_cursor(event->window, cursor);
    return TRUE;
}

void preview_progress(void *widget, char *text, double progress)
{
    if (widget==NULL) return;
    preview_data *data = get_preview_data(widget);
    if (data->ProgressBar==NULL) return;
    gtk_progress_bar_set_text(data->ProgressBar, text);
    gtk_progress_bar_set_fraction(data->ProgressBar, progress);
    while (gtk_events_pending()) gtk_main_iteration();
}

void window_response(GtkWidget *widget, gpointer user_data)
{
    preview_data *data = get_preview_data(widget);
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(widget));

    if (data->FreezeDialog) return;
    g_object_set_data(G_OBJECT(window), "WindowResponse", user_data);
    gtk_main_quit();
}

void preview_saver(GtkWidget *widget, ufraw_data *uf)
{
    preview_data *data = get_preview_data(widget);
    if (data->FreezeDialog==TRUE) return;
    gtk_widget_set_sensitive(data->ControlsBox, FALSE);
    data->FreezeDialog = TRUE;
    (*data->SaveFunc)(widget, uf);
    data->FreezeDialog = FALSE;
    gtk_widget_set_sensitive(data->ControlsBox, TRUE);
}

int ufraw_preview(ufraw_data *uf, int plugin, long (*save_func)())
{
    GtkWidget *previewWindow, *previewVBox;
    GtkTable *table, *subTable;
    GtkBox *previewHBox, *box, *hbox;
    GtkComboBox *combo;
    GtkWidget *button, *saveButton, *saveAsButton=NULL, *event_box, *align,
	    *label, *vBox, *noteBox, *page, *menu, *menu_item, *frame;
    GSList *group;
    GdkPixbuf *pixbuf;
    GdkRectangle screen;
    int max_preview_width, max_preview_height;
    int preview_width, preview_height, i;
    long j;
    int status, rowstride, curveeditorHeight;
    guint8 *pixies;
    char text[max_name];
    preview_data PreviewData;
    preview_data *data = &PreviewData;

    data->UF = uf;
    data->SaveFunc = save_func;

    data->SaveConfig = *uf->conf;
    data->SpotX1 = -1;
    data->FreezeDialog = TRUE;

    data->ToolTips = gtk_tooltips_new();
    previewWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    char *utf8_filename = g_filename_to_utf8(uf->filename,
	    -1, NULL, NULL, NULL);
    if (utf8_filename==NULL) utf8_filename = g_strdup("Unknown file name");
    char *previewHeader = g_strdup_printf("%s - UFRaw Photo Loader",
            utf8_filename);
    gtk_window_set_title(GTK_WINDOW(previewWindow), previewHeader);
    g_free(previewHeader);
    g_free(utf8_filename);

    gtk_window_set_icon(GTK_WINDOW(previewWindow),
            gdk_pixbuf_new_from_inline(-1, ufraw_icon, FALSE, NULL));
    gtk_window_set_resizable(GTK_WINDOW(previewWindow), FALSE);
    g_signal_connect(G_OBJECT(previewWindow), "delete-event",
            G_CALLBACK(window_delete_event), NULL);
    g_object_set_data(G_OBJECT(previewWindow), "Preview-Data", data);
    ufraw_focus(previewWindow, TRUE);
    uf->widget = previewWindow;

    gdk_screen_get_monitor_geometry(gdk_screen_get_default(), 0, &screen);
    max_preview_width = MIN(def_preview_width, screen.width-416);
    max_preview_height = MIN(def_preview_height, screen.height-120);
    CFG->Scale = MAX((uf->predictedWidth-1)/max_preview_width,
            (uf->predictedHeight-1)/max_preview_height)+1;
    CFG->Scale = MAX(2, CFG->Scale);
    CFG->Zoom = 100.0 / CFG->Scale;
    preview_width = uf->predictedWidth / CFG->Scale;
    preview_height = uf->predictedHeight / CFG->Scale;
    /* With the following guesses the window usually fits into the screen.
     * There should be more intelegent settings to widget sizes. */
    curveeditorHeight = 256;
    if (screen.height<801) {
	if (CFG->liveHistogramHeight==conf_default.liveHistogramHeight)
	    CFG->liveHistogramHeight = 96;
	if (CFG->rawHistogramHeight==conf_default.rawHistogramHeight)
	    CFG->rawHistogramHeight = 96;
	if (screen.height<800) curveeditorHeight = 192;
    }
    previewHBox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_container_add(GTK_CONTAINER(previewWindow), GTK_WIDGET(previewHBox));
    previewVBox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(previewHBox, previewVBox, TRUE, TRUE, 2);

    table = GTK_TABLE(table_with_frame(previewVBox, expanderText[raw_expander],
            CFG->expander[raw_expander]));
    event_box = gtk_event_box_new();
    gtk_table_attach(table, event_box, 0, 1, 1, 2, 0, 0, 0, 0);
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
            raw_his_size+2, CFG->rawHistogramHeight+2);
    data->RawHisto = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_container_add(GTK_CONTAINER(event_box), data->RawHisto);
    pixies = gdk_pixbuf_get_pixels(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    memset(pixies, 0, (gdk_pixbuf_get_height(pixbuf)-1)* rowstride +
            gdk_pixbuf_get_width(pixbuf)*gdk_pixbuf_get_n_channels(pixbuf));
    menu = gtk_menu_new();
    g_object_set_data(G_OBJECT(menu), "Parent-Widget", event_box);
    g_signal_connect_swapped(G_OBJECT(event_box), "button_press_event",
            G_CALLBACK(histogram_menu), menu);
    group = NULL;
    menu_item = gtk_radio_menu_item_new_with_label(group, "Linear");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 6, 7);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->rawHistogramScale==linear_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)linear_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->rawHistogramScale);
    menu_item = gtk_radio_menu_item_new_with_label(group, "Logarithmic");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 7, 8);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->rawHistogramScale==log_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)log_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->rawHistogramScale);

    menu_item = gtk_separator_menu_item_new();
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 8, 9);

    group = NULL;
    menu_item = gtk_radio_menu_item_new_with_label(group, "96 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 9, 10);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->rawHistogramHeight==96);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)96);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->rawHistogramHeight);
    menu_item = gtk_radio_menu_item_new_with_label(group, "128 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 10, 11);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->rawHistogramHeight==128);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)128);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->rawHistogramHeight);
    menu_item = gtk_radio_menu_item_new_with_label(group, "192 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 11, 12);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->rawHistogramHeight==192);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)192);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->rawHistogramHeight);
    menu_item = gtk_radio_menu_item_new_with_label(group, "256 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 12, 13);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->rawHistogramHeight==256);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)256);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->rawHistogramHeight);
    gtk_widget_show_all(menu);

    table = GTK_TABLE(table_with_frame(previewVBox, NULL, FALSE));
    data->SpotLabels = color_labels_new(table, 0, 1,
	    "Spot values:", pixel_format);
    data->SpotPatch = GTK_LABEL(gtk_label_new(NULL));
    gtk_table_attach_defaults(table, GTK_WIDGET(data->SpotPatch), 6, 7, 1, 2);

    table = GTK_TABLE(table_with_frame(previewVBox, NULL, FALSE));
    data->ExposureAdjustment = adjustment_scale(table, 0, 0, "Exposure",
            CFG->exposure, &CFG->exposure,
            -3, 3, 0.01, 1.0/6, 2, "EV compensation");

    button = gtk_toggle_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
            GTK_STOCK_CUT, GTK_ICON_SIZE_BUTTON));
    snprintf(text, max_name, "Highlight clipping\n"
	    "Current state: %s", CFG->unclip ? "unclip" : "clip");
    gtk_tooltips_set_tip(data->ToolTips, button, text, NULL);
    gtk_table_attach(table, button, 7, 8, 0, 1, 0, 0, 0, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), !CFG->unclip);
    g_signal_connect(G_OBJECT(button), "toggled",
            G_CALLBACK(toggle_button_update), &CFG->unclip);

    data->AutoExposureButton = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_container_add(GTK_CONTAINER(data->AutoExposureButton),
	    gtk_image_new_from_stock(GTK_STOCK_EXECUTE, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(data->AutoExposureButton),
	    "Auto adjust exposure", NULL);
    gtk_table_attach(table, GTK_WIDGET(data->AutoExposureButton), 8, 9, 0, 1,
	    0, 0, 0, 0);
    gtk_toggle_button_set_active(data->AutoExposureButton, CFG->autoExposure);
    g_signal_connect(G_OBJECT(data->AutoExposureButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->ResetExposureButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetExposureButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, data->ResetExposureButton,
	    "Reset exposure to default", NULL);
    gtk_table_attach(table, data->ResetExposureButton, 9, 10, 0, 1, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetExposureButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->ControlsBox = noteBox = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(previewVBox), noteBox, FALSE, FALSE, 0);

    /* Start of White Balance setting page */
    page = table_with_frame(noteBox, "WB", TRUE);
    /* Set this page to be the opening page. */
    int openingPage = gtk_notebook_page_num(GTK_NOTEBOOK(noteBox), page);

    table = GTK_TABLE(table_with_frame(page, NULL, TRUE));
    subTable = GTK_TABLE(gtk_table_new(10, 1, FALSE));
    gtk_table_attach(table, GTK_WIDGET(subTable), 0, 1, 0 , 1,
	    GTK_EXPAND|GTK_FILL, 0, 0, 0);
//    label = gtk_label_new("White balance");
//    gtk_table_attach(subTable, label, 0, 1, 0 , 1, 0, 0, 0, 0);
    data->WBCombo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    data->WBPresets = NULL;
    const wb_data *lastPreset = NULL;
    gboolean make_model_match = FALSE, make_model_fine_tuning = FALSE;
    for (i=0; i<wb_preset_count; i++) {
        if (strcmp(wb_preset[i].make, "")==0) {
	    /* Common presets */
	    data->WBPresets = g_list_append(data->WBPresets, wb_preset[i].name);
	    gtk_combo_box_append_text(data->WBCombo, wb_preset[i].name);
        } else if ( (strcmp(wb_preset[i].make, uf->conf->make)==0 ) &&
                    (strcmp(wb_preset[i].model, uf->conf->model)==0)) {
	    /* Camera specific presets */
            make_model_match = TRUE;
	    if ( lastPreset==NULL ||
		 strcmp(wb_preset[i].name,lastPreset->name)!=0 ) {
	    data->WBPresets = g_list_append(data->WBPresets, wb_preset[i].name);
		gtk_combo_box_append_text(data->WBCombo, wb_preset[i].name);
	    } else {
		/* Fine tuning presets */
		make_model_fine_tuning = TRUE;
	    }
	    lastPreset = &wb_preset[i];
        }
    }
    GList *l;
    gtk_combo_box_set_active(data->WBCombo, 0);
    for (i=0, l=data->WBPresets; g_list_next(l)!=NULL; i++, l=g_list_next(l))
	if (!strcmp(CFG->wb, l->data))
	    gtk_combo_box_set_active(data->WBCombo, i);
    g_signal_connect(G_OBJECT(data->WBCombo), "changed",
            G_CALLBACK(combo_update), CFG->wb);
    event_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(data->WBCombo));
    gtk_tooltips_set_tip(data->ToolTips, event_box, "White Balance", NULL);
    gtk_table_attach(subTable, event_box, 0, 6, 0, 1, GTK_FILL, 0, 0, 0);

    data->WBTuningAdjustment = NULL;
    if (make_model_fine_tuning) {
	data->WBTuningAdjustment = GTK_ADJUSTMENT(gtk_adjustment_new(
		CFG->WBTuning, -9, 9, 1, 1, 0));
	g_object_set_data(G_OBJECT(data->WBTuningAdjustment),
		"Adjustment-Accuracy",(gpointer)0);
	button = gtk_spin_button_new(data->WBTuningAdjustment, 1, 0);
    	g_object_set_data(G_OBJECT(data->WBTuningAdjustment),
		"Parent-Widget", button);
	gtk_table_attach(subTable, button, 6, 7, 0, 1, GTK_SHRINK, 0, 0, 0);
	g_signal_connect(G_OBJECT(data->WBTuningAdjustment), "value-changed",
		G_CALLBACK(adjustment_update), &CFG->WBTuning);
    } else if (!make_model_match) {
	event_box = gtk_event_box_new();
	label = gtk_image_new_from_stock(GTK_STOCK_DIALOG_WARNING,
		GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(event_box), label);
	gtk_table_attach(subTable, event_box, 6, 7, 0, 1, GTK_FILL, 0, 0, 0);
	gtk_tooltips_set_tip(data->ToolTips, event_box,
	    "There are no white balance presets for your camera model.\n"
	    "Check UFRaw's webpage for information on how to get your\n"
	    "camera supported.", NULL);
    }
    data->ResetWBButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetWBButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, data->ResetWBButton,
	    "Reset white balance to initial value", NULL);
    gtk_table_attach(subTable, data->ResetWBButton, 7, 8, 0, 1, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetWBButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->TemperatureAdjustment = adjustment_scale(subTable, 0, 1,
            "Temperature", CFG->temperature, &CFG->temperature,
            2000, 12000, 50, 200, 0,
            "White balance color temperature (K)");
    data->GreenAdjustment = adjustment_scale(subTable, 0, 2, "Green",
            CFG->green, &CFG->green, 0.2, 2.5, 0.01, 0.05, 2,
            "Green component");
    // Spot WB button:
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
                GTK_STOCK_COLOR_PICKER, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, button,
	    "Select a spot on the preview image to apply spot white balance",
	    NULL);
    gtk_table_attach(subTable, button, 7, 8, 1, 3, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(spot_wb_event), NULL);

    GtkBox *subbox = GTK_BOX(gtk_hbox_new(0,0));
    gtk_table_attach(table, GTK_WIDGET(subbox), 0, 1, 1, 2, 0, 0, 0, 0);
    if (data->UF->colors>3)
	label = gtk_label_new("Chan. multipliers:");
    else
	label = gtk_label_new("Channel multipliers:");
    gtk_box_pack_start(subbox, label, 0, 0, 0);
    for (i=0; i<data->UF->colors; i++) {
	data->ChannelAdjustment[i] = GTK_ADJUSTMENT(gtk_adjustment_new(
		CFG->chanMul[i], 0.5, 9.0, 0.01, 0.01, 0));
	g_object_set_data(G_OBJECT(data->ChannelAdjustment[i]),
		"Adjustment-Accuracy",(gpointer)2);
	button = gtk_spin_button_new(data->ChannelAdjustment[i], 0.01, 2);
    	g_object_set_data(G_OBJECT(data->ChannelAdjustment[i]),
		"Parent-Widget", button);
	gtk_box_pack_start(subbox, button, 0, 0, 0);
	g_signal_connect(G_OBJECT(data->ChannelAdjustment[i]), "value-changed",
		G_CALLBACK(adjustment_update), &CFG->chanMul[i]);
    }
    /* End of White Balance setting page */

    /* Start of Base Curve page */
    page = table_with_frame(noteBox, "Base", TRUE);

    table = GTK_TABLE(table_with_frame(page, NULL, TRUE));
    box = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_table_attach(table, GTK_WIDGET(box), 0, 9, 0, 1,
	    GTK_EXPAND|GTK_FILL, 0, 0, 0);

    data->BaseCurveCombo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    /* Fill in the curve names, skipping custom and camera curves if there is
     * no cameraCurve. This will make some mess later with the counting */
    for (i=0; i<CFG->BaseCurveCount; i++)
        if ( (i!=custom_curve && i!=camera_curve) || CFG_cameraCurve )
            gtk_combo_box_append_text(data->BaseCurveCombo,
		    CFG->BaseCurve[i].name);
    /* This is part of the mess with the combo_box counting */
    if (CFG->BaseCurveIndex>camera_curve && !CFG_cameraCurve)
        gtk_combo_box_set_active(data->BaseCurveCombo, CFG->BaseCurveIndex-2);
    else
        gtk_combo_box_set_active(data->BaseCurveCombo, CFG->BaseCurveIndex);
    g_signal_connect(G_OBJECT(data->BaseCurveCombo), "changed",
            G_CALLBACK(combo_update), &CFG->BaseCurveIndex);
    gtk_box_pack_start(box, GTK_WIDGET(data->BaseCurveCombo), TRUE, TRUE, 0);
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
            GTK_STOCK_OPEN, GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_start(box, button, FALSE, FALSE, 0);
    gtk_tooltips_set_tip(data->ToolTips, button, "Load base curve", NULL);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(load_curve), (gpointer)base_curve);
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
            GTK_STOCK_SAVE_AS, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, button, "Save base curve", NULL);
    gtk_box_pack_start(box, button, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(save_curve), (gpointer)base_curve);

    box = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_table_attach(table, GTK_WIDGET(box), 0, 9, 1, 2,
	    GTK_EXPAND|GTK_FILL, 0, 0, 0);
    data->BaseCurveWidget = curveeditor_widget_new(curveeditorHeight, 256,
	    curve_update, (gpointer)base_curve);
    curveeditor_widget_set_curve(data->BaseCurveWidget,
	    &CFG->BaseCurve[CFG->BaseCurveIndex]);
    align = gtk_alignment_new(1, 0, 0, 1);
    gtk_container_add(GTK_CONTAINER(align), GTK_WIDGET(data->BaseCurveWidget));
    gtk_box_pack_start(box, align, TRUE, TRUE, 0);

    data->ResetBaseCurveButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetBaseCurveButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(data->ResetBaseCurveButton),
	    "Reset base curve to default",NULL);
    align = gtk_alignment_new(0, 1, 1, 0);
    gtk_container_add(GTK_CONTAINER(align),
	    GTK_WIDGET(data->ResetBaseCurveButton));
    gtk_box_pack_start(box, align, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(data->ResetBaseCurveButton), "clicked",
            G_CALLBACK(button_update), NULL);
    /* End of Base Curve page */

    /* Start of Color management page */
    page = table_with_frame(noteBox, "Color", TRUE);
    table = GTK_TABLE(table_with_frame(page, NULL, TRUE));
    for (j=0; j<profile_types; j++) {
        label = gtk_label_new(j==in_profile ? "Input profile" :
                j==out_profile ? "Output profile" : "Error");
        gtk_table_attach(table, label, 0, 1, 4*j+1, 4*j+2, 0, 0, 0, 0);
        data->ProfileCombo[j] = GTK_COMBO_BOX(gtk_combo_box_new_text());
        for (i=0; i<CFG->profileCount[j]; i++)
            gtk_combo_box_append_text(data->ProfileCombo[j],
                    CFG->profile[j][i].name);
        gtk_combo_box_set_active(data->ProfileCombo[j], CFG->profileIndex[j]);
        g_signal_connect(G_OBJECT(data->ProfileCombo[j]), "changed",
                G_CALLBACK(combo_update), &CFG->profileIndex[j]);
        gtk_table_attach(table, GTK_WIDGET(data->ProfileCombo[j]),
                1, 7, 4*j+1, 4*j+2, GTK_FILL, GTK_FILL, 0, 0);
        button = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
                GTK_STOCK_OPEN, GTK_ICON_SIZE_BUTTON));
        gtk_table_attach(table, button, 7, 8, 4*j+1, 4*j+2,
                GTK_SHRINK, GTK_FILL, 0, 0);
        g_signal_connect(G_OBJECT(button), "clicked",
                G_CALLBACK(load_profile), (void *)j);
    }
    data->UseMatrixButton = gtk_check_button_new_with_label("Use color matrix");
    gtk_table_attach(table, data->UseMatrixButton, 0, 3, 2, 3, 0, 0, 0, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->UseMatrixButton),
	    data->UF->useMatrix);
    g_signal_connect(G_OBJECT(data->UseMatrixButton), "toggled",
            G_CALLBACK(toggle_button_update), &data->UF->useMatrix);
    if (data->UF->raw_color || data->UF->colors==4)
	gtk_widget_set_sensitive(data->UseMatrixButton, FALSE);

    data->GammaAdjustment = adjustment_scale(table, 0, 3, "Gamma",
            CFG->profile[0][CFG->profileIndex[0]].gamma,
            &CFG->profile[0][0].gamma, 0.1, 1.0, 0.01, 0.05, 2,
            "Gamma correction for the input profile");
    data->ResetGammaButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetGammaButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, data->ResetGammaButton,
	    "Reset gamma to default", NULL);
    gtk_table_attach(table, data->ResetGammaButton, 7, 8, 3, 4, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetGammaButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->LinearAdjustment = adjustment_scale(table, 0, 4, "Linearity",
            CFG->profile[0][CFG->profileIndex[0]].linear,
            &CFG->profile[0][0].linear, 0.0, 1.0, 0.01, 0.05, 2,
            "Linear part of the gamma correction");
    data->ResetLinearButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetLinearButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, data->ResetLinearButton,
	    "Reset linearity to default", NULL);
    gtk_table_attach(table, data->ResetLinearButton, 7, 8, 4, 5, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetLinearButton), "clicked",
            G_CALLBACK(button_update), NULL);

    label = gtk_label_new("Intent");
    gtk_table_attach(table, label, 0, 1, 6, 7, 0, 0, 0, 0);
    combo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    gtk_combo_box_append_text(combo, "Perceptual");
    gtk_combo_box_append_text(combo, "Relative colorimetric");
    gtk_combo_box_append_text(combo, "Saturation");
    gtk_combo_box_append_text(combo, "Absolute colorimetric");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), CFG->intent);
    g_signal_connect(G_OBJECT(combo), "changed",
            G_CALLBACK(combo_update), &CFG->intent);
    gtk_table_attach(table, GTK_WIDGET(combo), 1, 7, 6, 7, GTK_FILL, 0, 0, 0);
    /* End of Color management page */

    /* Start of Corrections page */
    page = table_with_frame(noteBox, "Corrections", TRUE);

    table = GTK_TABLE(table_with_frame(page, NULL, TRUE));
    data->SaturationAdjustment = adjustment_scale(table, 0, 1, "Saturation",
            CFG->saturation, &CFG->saturation,
            0.0, 3.0, 0.01, 0.1, 2, "Saturation");
    data->ResetSaturationButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetSaturationButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, data->ResetSaturationButton,
	    "Reset saturation to default", NULL);
    gtk_table_attach(table, data->ResetSaturationButton, 9, 10, 1, 2,
	    0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetSaturationButton), "clicked",
            G_CALLBACK(button_update), NULL);

    table = GTK_TABLE(table_with_frame(page, NULL,
            CFG->expander[curve_expander]));
    box = GTK_BOX(gtk_hbox_new(FALSE, 0));
    gtk_table_attach(table, GTK_WIDGET(box), 0, 9, 0, 1,
	    GTK_EXPAND|GTK_FILL, 0, 0, 0);
    data->CurveCombo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    /* Fill in the curve names */
    for (i=0; i<CFG->curveCount; i++)
        gtk_combo_box_append_text(data->CurveCombo, CFG->curve[i].name);
    gtk_combo_box_set_active(data->CurveCombo, CFG->curveIndex);
    g_signal_connect(G_OBJECT(data->CurveCombo), "changed",
            G_CALLBACK(combo_update), &CFG->curveIndex);
    gtk_box_pack_start(box, GTK_WIDGET(data->CurveCombo), TRUE, TRUE, 0);
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
            GTK_STOCK_OPEN, GTK_ICON_SIZE_BUTTON));
    gtk_box_pack_start(box, button, FALSE, FALSE, 0);
    gtk_tooltips_set_tip(data->ToolTips, button, "Load curve", NULL);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(load_curve), (gpointer)luminosity_curve);
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
            GTK_STOCK_SAVE_AS, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, button, "Save curve", NULL);
    gtk_box_pack_start(box, button, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(save_curve), (gpointer)luminosity_curve);

    subTable = GTK_TABLE(gtk_table_new(9, 9, FALSE));
    gtk_table_attach(table, GTK_WIDGET(subTable), 0, 1, 1 , 2,
	    GTK_EXPAND|GTK_FILL, 0, 0, 0);
    data->CurveWidget = curveeditor_widget_new(curveeditorHeight, 256,
	    curve_update, (gpointer)luminosity_curve);
    curveeditor_widget_set_curve(data->CurveWidget,
	    &CFG->curve[CFG->curveIndex]);
    gtk_table_attach(subTable, data->CurveWidget, 1, 8, 1, 8,
	    GTK_EXPAND|GTK_FILL, 0, 0, 0);

    data->AutoCurveButton = GTK_BUTTON(gtk_button_new());
    gtk_container_add(GTK_CONTAINER(data->AutoCurveButton),
	    gtk_image_new_from_stock(GTK_STOCK_EXECUTE, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(data->AutoCurveButton),
	    "Auto adjust curve\n(Flatten histogram)", NULL);
    gtk_table_attach(subTable, GTK_WIDGET(data->AutoCurveButton), 8, 9, 6, 7,
	    0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->AutoCurveButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->ResetCurveButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetCurveButton),
	    gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(data->ResetCurveButton),
	    "Reset curve to default",NULL);
    gtk_table_attach(subTable, GTK_WIDGET(data->ResetCurveButton), 8, 9, 7, 8,
		0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetCurveButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->BlackLabel = gtk_label_new("Black point: 0.000");
#ifdef HAVE_GTK_2_6
    gtk_misc_set_alignment(GTK_MISC(data->BlackLabel), 0.5, 1.0);
    gtk_label_set_angle(GTK_LABEL(data->BlackLabel), 90);
    gtk_table_attach(subTable, data->BlackLabel, 0, 1, 5, 6,
	    0, GTK_FILL|GTK_EXPAND, 0, 0);
#else
    button = gtk_alignment_new(0, 0, 0, 0);
    gtk_table_attach(subTable, button, 0, 1, 5, 6,
	    0, GTK_FILL|GTK_EXPAND, 0, 0);
    gtk_misc_set_alignment(GTK_MISC(data->BlackLabel), 0.0, 0.5);
    gtk_table_attach(subTable, data->BlackLabel, 1, 8, 8, 9, GTK_FILL, 0, 0, 0);
#endif
    data->ResetBlackButton = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(data->ResetBlackButton),
	gtk_image_new_from_stock(GTK_STOCK_REFRESH, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(data->ResetBlackButton),
	    "Reset black-point to default",NULL);
    gtk_table_attach(subTable, GTK_WIDGET(data->ResetBlackButton), 0, 1, 7, 8,
	    0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ResetBlackButton), "clicked",
            G_CALLBACK(button_update), NULL);

    data->AutoBlackButton = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_container_add(GTK_CONTAINER(data->AutoBlackButton),
	    gtk_image_new_from_stock(GTK_STOCK_EXECUTE, GTK_ICON_SIZE_BUTTON));
    gtk_tooltips_set_tip(data->ToolTips, GTK_WIDGET(data->AutoBlackButton),
	    "Auto adjust black-point", NULL);
    gtk_table_attach(subTable, GTK_WIDGET(data->AutoBlackButton), 0, 1, 6, 7,
	    0, GTK_SHRINK, 0, 0);
    gtk_toggle_button_set_active(data->AutoBlackButton, CFG->autoBlack);
    g_signal_connect(G_OBJECT(data->AutoBlackButton), "clicked",
            G_CALLBACK(button_update), NULL);
    /* End of Corrections page */

    /* Start of Zoom page */
    page = table_with_frame(noteBox, "Zoom", TRUE);

    table = GTK_TABLE(table_with_frame(page, NULL, TRUE));

    label = gtk_label_new("Zoom %");
    gtk_table_attach(table, label, 0, 1, 0, 1, 0, 0, 0, 0);
    data->ZoomAdjustment = GTK_ADJUSTMENT(gtk_adjustment_new(
		CFG->Zoom, 5, 50, 1, 1, 0));
    g_object_set_data(G_OBJECT(data->ZoomAdjustment),
		"Adjustment-Accuracy", (gpointer)0);
    button = gtk_spin_button_new(data->ZoomAdjustment, 1, 0);
    g_object_set_data(G_OBJECT(data->ZoomAdjustment), "Parent-Widget", button);
    gtk_table_attach(table, button, 1, 2, 0, 1, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(data->ZoomAdjustment), "value-changed",
		G_CALLBACK(adjustment_update), &CFG->Zoom);
    // Zoom in button:
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
                GTK_STOCK_ZOOM_IN, GTK_ICON_SIZE_BUTTON));
    gtk_table_attach(table, button, 2, 3, 0, 1, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(zoom_in_event), NULL);
    // Zoom out button:
    button = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(button), gtk_image_new_from_stock(
                GTK_STOCK_ZOOM_OUT, GTK_ICON_SIZE_BUTTON));
    gtk_table_attach(table, button, 3, 4, 0, 1, 0, 0, 0, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(zoom_out_event), NULL);
    /* End of Zoom page */

    /* Start of EXIF page */
    page = table_with_frame(noteBox, "EXIF", TRUE);
    frame = gtk_frame_new(NULL);
    gtk_box_pack_start(GTK_BOX(page), frame, TRUE, TRUE, 0);
    table = GTK_TABLE(gtk_table_new(10, 10, FALSE));
    gtk_container_add(GTK_CONTAINER(frame), GTK_WIDGET(table));

    align = gtk_scrolled_window_new(NULL, NULL);
    gtk_table_attach(table, align, 0, 1, 0, 1, GTK_EXPAND|GTK_FILL,
	    GTK_EXPAND|GTK_FILL, 0, 0);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(align),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    label = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(align), label);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(label), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(label), FALSE);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(label));
    GString *message = g_string_new("");
    g_string_append_printf(message, "Camera: %s %s\n", CFG->make, CFG->model);
    g_string_append_printf(message, "Timestamp: %s\n", CFG->timestamp);
    g_string_append_printf(message, "Shutter: %s\n", CFG->shutterText);
    g_string_append_printf(message, "Aperture: %s\n", CFG->apertureText);
    g_string_append_printf(message, "ISO speed: %s\n", CFG->isoText);
    g_string_append_printf(message, "Focal length: %s\n",
	    CFG->focalLenText);
    if (strlen(CFG->lensText)>0)
	g_string_append_printf(message, "Lens: %s\n", CFG->lensText);
    g_string_append_printf(message, "\nEXIF data read by %s", CFG->exifSource);
    gtk_text_buffer_set_text(buffer, message->str, -1);
    g_string_free(message, TRUE);
    if (uf->exifBuf==NULL) {
	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), "<span foreground='red'>"
		"Warning: EXIF data will not be sent to output</span>");
	gtk_table_attach(table, label, 0, 1, 1, 2, 0, 0, 0, 0);
    }
    /* End of EXIF page */

    table = GTK_TABLE(table_with_frame(previewVBox, expanderText[live_expander],
            CFG->expander[live_expander]));
    event_box = gtk_event_box_new();
    gtk_table_attach(table, event_box, 0, 7, 1, 2, 0, 0, 0, 0);
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
            live_his_size+2, CFG->liveHistogramHeight+2);
    data->LiveHisto = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_container_add(GTK_CONTAINER(event_box), data->LiveHisto);
    pixies = gdk_pixbuf_get_pixels(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    memset(pixies, 0, (gdk_pixbuf_get_height(pixbuf)-1)* rowstride +
            gdk_pixbuf_get_width(pixbuf)*gdk_pixbuf_get_n_channels(pixbuf));
    menu = gtk_menu_new();
    g_object_set_data(G_OBJECT(menu), "Parent-Widget", event_box);
    g_signal_connect_swapped(G_OBJECT(event_box), "button_press_event",
            G_CALLBACK(histogram_menu), menu);
    group = NULL;
    menu_item = gtk_radio_menu_item_new_with_label(group, "RGB histogram");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 0, 1);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->histogram==rgb_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)rgb_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->histogram);
    menu_item = gtk_radio_menu_item_new_with_label(group, "R+G+B histogram");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 1, 2);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->histogram==r_g_b_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)r_g_b_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->histogram);
    menu_item = gtk_radio_menu_item_new_with_label(group,
	    "Luminosity histogram");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 2, 3);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->histogram==luminosity_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)luminosity_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->histogram);
    menu_item = gtk_radio_menu_item_new_with_label(group,
	    "Value (maximum) histogram");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 3, 4);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->histogram==value_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)value_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->histogram);
    menu_item = gtk_radio_menu_item_new_with_label(group,
	    "Saturation histogram");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 4, 5);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->histogram==saturation_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)saturation_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->histogram);

    menu_item = gtk_separator_menu_item_new();
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 5, 6);

    group = NULL;
    menu_item = gtk_radio_menu_item_new_with_label(group, "Linear");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 6, 7);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->liveHistogramScale==linear_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)linear_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->liveHistogramScale);
    menu_item = gtk_radio_menu_item_new_with_label(group, "Logarithmic");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 7, 8);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->liveHistogramScale==log_histogram);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)log_histogram);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->liveHistogramScale);

    menu_item = gtk_separator_menu_item_new();
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 8, 9);

    group = NULL;
    menu_item = gtk_radio_menu_item_new_with_label(group, "96 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 9, 10);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->liveHistogramHeight==96);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)96);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->liveHistogramHeight);
    menu_item = gtk_radio_menu_item_new_with_label(group, "128 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 10, 11);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->liveHistogramHeight==128);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)128);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->liveHistogramHeight);
    menu_item = gtk_radio_menu_item_new_with_label(group, "192 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 11, 12);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->liveHistogramHeight==192);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)192);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->liveHistogramHeight);
    menu_item = gtk_radio_menu_item_new_with_label(group, "256 Pixels");
    gtk_menu_attach(GTK_MENU(menu), menu_item, 0, 1, 12, 13);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(menu_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item),
	    CFG->liveHistogramHeight==256);
    g_object_set_data(G_OBJECT(menu_item), "Radio-Value",
	    (gpointer)256);
    g_signal_connect(G_OBJECT(menu_item), "toggled",
            G_CALLBACK(radio_menu_update), &CFG->liveHistogramHeight);
    gtk_widget_show_all(menu);

    i = 2;
    data->AvrLabels = color_labels_new(table, 0, i++, "Average:", pixel_format);
    data->DevLabels = color_labels_new(table, 0, i++, "Std. deviation:",
            pixel_format);
    data->OverLabels = color_labels_new(table, 0, i,
	    "Overexposed:", percent_format);
    toggle_button(table, 4, i, NULL, &CFG->overExp);
    button = gtk_button_new_with_label("Indicate");
    gtk_table_attach(table, button, 6, 7, i, i+1,
            GTK_SHRINK|GTK_FILL, GTK_FILL, 0, 0);
    g_signal_connect(G_OBJECT(button), "pressed",
            G_CALLBACK(render_preview_callback), (void *)render_overexposed);
    g_signal_connect(G_OBJECT(button), "released",
            G_CALLBACK(render_preview_callback), (void *)render_default);
    i++;
    data->UnderLabels = color_labels_new(table, 0, i, "Underexposed:",
            percent_format);
    toggle_button(table, 4, i, NULL, &CFG->underExp);
    button = gtk_button_new_with_label("Indicate");
    gtk_table_attach(table, button, 6, 7, i, i+1,
            GTK_SHRINK|GTK_FILL, GTK_FILL, 0, 0);
    g_signal_connect(G_OBJECT(button), "pressed",
            G_CALLBACK(render_preview_callback), (void *)render_underexposed);
    g_signal_connect(G_OBJECT(button), "released",
            G_CALLBACK(render_preview_callback), (void *)render_default);
    i++;

    /* Right side of the preview window */
    vBox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(previewHBox, vBox, FALSE, FALSE, 2);
    align = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_box_pack_start(GTK_BOX(vBox), align, TRUE, TRUE, 0);
    box = GTK_BOX(gtk_vbox_new(FALSE, 0));
    gtk_container_add(GTK_CONTAINER(align), GTK_WIDGET(box));
    event_box = gtk_event_box_new();
    gtk_box_pack_start(box, event_box, FALSE, FALSE, 0);
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
            preview_width, preview_height);
    data->PreviewWidget = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    gtk_misc_set_alignment(GTK_MISC(data->PreviewWidget), 0, 0);
    gtk_container_add(GTK_CONTAINER(event_box), data->PreviewWidget);
    g_signal_connect(G_OBJECT(event_box), "button_press_event",
            G_CALLBACK(spot_press), NULL);
    g_signal_connect(G_OBJECT(event_box), "motion-notify-event",
            G_CALLBACK(spot_motion), NULL);
    g_signal_connect(G_OBJECT(event_box), "enter-notify-event",
	    G_CALLBACK(preview_cursor), gdk_cursor_new(GDK_HAND2));
    g_signal_connect(G_OBJECT(event_box), "leave-notify-event",
	    G_CALLBACK(preview_cursor), NULL);

    data->ProgressBar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_box_pack_start(box, GTK_WIDGET(data->ProgressBar), FALSE, FALSE, 0);

    box = GTK_BOX(gtk_hbox_new(FALSE, 6));
    gtk_box_pack_start(GTK_BOX(vBox), GTK_WIDGET(box), FALSE, FALSE, 6);
    combo = GTK_COMBO_BOX(gtk_combo_box_new_text());
    gtk_combo_box_append_text(combo, "AHD interpolation");
    gtk_combo_box_append_text(combo, "VNG interpolation");
    gtk_combo_box_append_text(combo, "VNG four color interpolation");
    gtk_combo_box_append_text(combo, "Bilinear interpolation");
    if (plugin) {
        gtk_combo_box_append_text(combo, "Half-size interpolation");
    }
    gtk_combo_box_set_active(combo, CFG->interpolation);
    g_signal_connect(G_OBJECT(combo), "changed",
            G_CALLBACK(combo_update), &CFG->interpolation);
    gtk_box_pack_start(box, GTK_WIDGET(combo), FALSE, FALSE, 0);

    /* Options button */
    align = gtk_alignment_new(0.99, 0.5, 0, 1);
    gtk_box_pack_start(box, align, TRUE, TRUE, 6);
    box = GTK_BOX(gtk_hbox_new(TRUE, 6));
    gtk_container_add(GTK_CONTAINER(align), GTK_WIDGET(box));
    button = gtk_button_new();
    hbox = GTK_BOX(gtk_hbox_new(FALSE, 6));
    gtk_container_add(GTK_CONTAINER(button), GTK_WIDGET(hbox));
    gtk_box_pack_start(hbox, gtk_image_new_from_stock(
            GTK_STOCK_PREFERENCES, GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
    gtk_box_pack_start(hbox, gtk_label_new("Options"),
            FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(options_dialog), previewWindow);
    gtk_box_pack_start(box, button, TRUE, TRUE, 0);
    button = gtk_button_new_from_stock(GTK_STOCK_CANCEL);
    g_signal_connect(G_OBJECT(button), "clicked",
            G_CALLBACK(window_response), (gpointer)GTK_RESPONSE_CANCEL);
    gtk_box_pack_start(box, button, TRUE, TRUE, 0);
    if (plugin) {
	saveButton = gtk_button_new_from_stock(GTK_STOCK_OK);
        gtk_box_pack_start(box, saveButton, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(saveButton), "clicked",
		G_CALLBACK(preview_saver), uf);
        gtk_widget_grab_focus(saveButton);
    } else {
	saveButton = gtk_button_new_from_stock(GTK_STOCK_SAVE);
        gtk_box_pack_start(box, saveButton, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(saveButton), "clicked",
		G_CALLBACK(preview_saver), uf);

	/* Get the default save options from ufraw_saver() */
	char *text = (char *)ufraw_saver(NULL, uf);
	gtk_tooltips_set_tip(data->ToolTips, saveButton, text, NULL);
	g_free(text);

	saveAsButton = gtk_button_new_from_stock(GTK_STOCK_SAVE_AS);
        gtk_box_pack_start(box, saveAsButton, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(saveAsButton), "clicked",
		G_CALLBACK(preview_saver), uf);
        gtk_widget_grab_focus(saveAsButton);
    }
    gtk_widget_show_all(previewWindow);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(noteBox), openingPage);

    gtk_widget_set_sensitive(data->ControlsBox, FALSE);
    preview_progress(previewWindow, "Loading preview", 0.2);
    ufraw_load_raw(uf);
    gtk_widget_set_sensitive(data->ControlsBox, TRUE);

    create_base_image(data);

    /* Save InitialChanMul[] for the sake of "Reset WB" */
    for (i=0; i<4; i++) data->initialChanMul[i] = CFG->chanMul[i];

    /* Update the curve editor in case ufraw_convert_image() modified it. */
    curveeditor_widget_set_curve(data->CurveWidget,
	    &CFG->curve[CFG->curveIndex]);

    data->FreezeDialog = FALSE;
    update_scales(data);

    gtk_main();
    status = (long)g_object_get_data(G_OBJECT(previewWindow),
            "WindowResponse");
    gtk_container_foreach(GTK_CONTAINER(previewVBox),
            (GtkCallback)(expander_state), NULL);
    ufraw_focus(previewWindow, FALSE);
    gtk_widget_destroy(previewWindow);
    gtk_object_sink(GTK_OBJECT(data->ToolTips));
    /* Make sure that there are no preview idle task remaining */
    g_idle_remove_by_data(data);

    /* In interactive mode outputPath is taken into account only once */
    strcpy(uf->conf->outputPath, "");
    if (status==GTK_RESPONSE_OK) {
	if (CFG->saveConfiguration==disabled_state) {
	    /* Restore the original 'image settings' */
	    conf_copy_image(CFG, &data->SaveConfig);
	}
	/* If save 'only this once' was chosen, then so be it */
	if (CFG->saveConfiguration==apply_state)
	    CFG->saveConfiguration = disabled_state;
	conf_save(CFG, NULL, NULL);
    } else {
	*CFG = data->SaveConfig;
    }
    ufraw_close(data->UF);
    g_list_free(data->WBPresets);

    if (status!=GTK_RESPONSE_OK) return UFRAW_CANCEL;
    return UFRAW_SUCCESS;
}
