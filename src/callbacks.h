#include <gtk/gtk.h>

void on_button_stats_update_clicked (GtkButton *button, gpointer user_data);
void on_button_nodes_remove_clicked (GtkButton *button, gpointer user_data);
void on_button_nodes_add_clicked (GtkButton *button, gpointer user_data); 
void on_button_host_catcher_connect_clicked (GtkButton *button, gpointer user_data); 
void on_button_host_catcher_get_more_clicked (GtkButton *button, gpointer user_data);
void on_button_host_catcher_remove_clicked (GtkButton *button, gpointer user_data);
void on_button_host_catcher_clear_clicked (GtkButton *button, gpointer user_data);
void on_clist_nodes_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_entry_up_connections_activate (GtkEditable *editable, gpointer user_data); 
gboolean on_entry_up_connections_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_clist_host_catcher_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_clist_uploads_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data); 
void on_button_kill_upload_clicked (GtkButton *button, gpointer user_data); 
void on_button_clear_uploads_clicked (GtkButton *button, gpointer user_data);
void on_checkbutton_clear_uploads_toggled (GtkToggleButton *togglebutton, gpointer user_data);
void on_button_abort_download_clicked (GtkButton *button, gpointer user_data); 
void on_button_resume_download_clicked (GtkButton *button, gpointer user_data); 
void on_button_clear_download_clicked (GtkButton *button, gpointer user_data); 
void on_entry_max_downloads_activate (GtkEditable *editable, gpointer user_data); 
void on_checkbutton_clear_downloads_toggled (GtkToggleButton *togglebutton, gpointer user_data);
void on_button_remove_upload_clicked (GtkButton *button, gpointer user_data); 
void on_button_search_clicked (GtkButton *button, gpointer user_data); 
void on_entry_minimum_speed_activate (GtkEditable *editable, gpointer user_data); 
void on_clist_search_results_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data); 
void on_button_search_download_clicked (GtkButton *button, gpointer user_data); 
void on_button_search_stream_clicked (GtkButton *button, gpointer user_data); 
void on_checkbutton_monitor_toggled (GtkToggleButton *togglebutton, gpointer user_data); 
void on_entry_monitor_activate (GtkEditable *editable, gpointer user_data); 
void on_button_config_save_path_clicked (GtkButton *button, gpointer user_data); 
void on_button_config_add_dir_clicked (GtkButton *button, gpointer user_data); 
void on_button_config_rescan_dir_clicked (GtkButton *button, gpointer user_data); 
void on_entry_config_extensions_changed (GtkEditable *editable, gpointer user_data);
void on_checkbutton_config_force_ip_toggled (GtkToggleButton *togglebutton, gpointer user_data);
void on_entry_config_force_ip_changed (GtkEditable *editable, gpointer user_data);
void on_button_config_update_port_clicked (GtkButton *button, gpointer user_data); 
void on_checkbutton_config_throttle_toggled (GtkToggleButton *togglebutton, gpointer user_data);
void on_entry_config_maxttl_changed (GtkEditable *editable, gpointer user_data);
void on_entry_config_myttl_changed (GtkEditable *editable, gpointer user_data);
void on_clist_menu_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_entry_host_changed (GtkEditable *editable, gpointer user_data);
void on_clist_nodes_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_clist_host_catcher_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data); 
void on_clist_uploads_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_clist_downloads_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_clist_downloads_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
gboolean on_entry_max_downloads_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_clist_download_queue_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_clist_download_queue_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
gboolean on_entry_minimum_speed_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_clist_search_results_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
gboolean on_entry_monitor_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data); 
void on_button_config_save_path_clicked (GtkButton *button, gpointer user_data);
void on_entry_config_search_items_activate (GtkEditable *editable, gpointer user_data);
gboolean on_entry_config_search_items_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_speed_activate (GtkEditable *editable, gpointer user_data);
gboolean on_entry_config_speed_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_search_changed (GtkEditable *editable, gpointer user_data);
void on_entry_config_port_changed (GtkEditable *editable, gpointer user_data);
gboolean on_clist_host_catcher_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_entry_monitor_activate (GtkEditable *editable, gpointer user_data); 
void on_button_extra_config_clicked (GtkButton *button, gpointer user_data); 
void on_entry_host_activate (GtkEditable *editable, gpointer user_data); 
gboolean on_main_window_destroy_event (GtkWidget *widget, GdkEvent *event, gpointer user_data); 
gboolean on_main_window_delete_event (GtkWidget *widget, GdkEvent *event, gpointer user_data);
void on_entry_max_downloads_activate (GtkEditable *editable, gpointer user_data); 
void on_entry_search_activate (GtkEditable *editable, gpointer user_data); 
void on_entry_config_extensions_activate (GtkEditable *editable, gpointer user_data); 
gboolean on_entry_config_extensions_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_maxttl_activate (GtkEditable *editable, gpointer user_data); 
gboolean on_entry_config_maxttl_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data); 
void on_entry_config_myttl_activate (GtkEditable *editable, gpointer user_data);
gboolean on_entry_config_myttl_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_force_ip_activate (GtkEditable *editable, gpointer user_data); 
void on_entry_config_port_activate (GtkEditable *editable, gpointer user_data); 
gboolean on_entry_config_force_ip_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_entry_config_path_activate (GtkEditable *editable, gpointer user_data); 
gboolean on_entry_config_path_focus_out_event (GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
void on_button_remove_download_clicked (GtkButton *button, gpointer user_data); 
gboolean on_clist_search_results_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_clist_uploads_click_column (GtkCList *clist, gint column, gpointer user_data); 
void on_clist_downloads_click_column (GtkCList *clist, gint column, gpointer user_data); 
void on_clist_download_queue_click_column (GtkCList *clist, gint column, gpointer user_data);
void on_clist_search_results_click_column (GtkCList *clist, gint column, gpointer user_data);
void on_download_start_now_activate (GtkMenuItem *menuitem, gpointer user_data);
gboolean on_clist_downloads_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean on_clist_download_queue_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_download_p_queue_activate (GtkMenuItem *menuitem, gpointer user_data); 
void on_download_p_kill_activate (GtkMenuItem *menuitem, gpointer user_data);
void on_popup_hosts_export_activate (GtkMenuItem *menuitem, gpointer user_data);
void on_popup_hosts_importe_activate (GtkMenuItem *menuitem, gpointer user_data);
void on_download_p_push_activate (GtkMenuItem *menuitem, gpointer user_data);
void on_popup_monitor_title_activate (GtkMenuItem *menuitem, gpointer user_data);
gboolean on_clist_monitor_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_popup_nodes_title_activate (GtkMenuItem *menuitem, gpointer user_data); 
gboolean on_clist_nodes_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_button_config_move_path_clicked (GtkButton *button, gpointer user_data); 
void on_popup_uploads_title_activate (GtkMenuItem *menuitem, gpointer user_data);
gboolean on_clist_uploads_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_popup_search_stop_sorting_activate (GtkMenuItem *menuitem, gpointer user_data);
void on_button_search_filter_clicked (GtkButton *button, gpointer user_data);
void on_button_search_close_clicked (GtkButton *button, gpointer user_data);
