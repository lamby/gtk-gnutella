
#include <ctype.h>

/* Handle searches */

#include "gnutella.h"

#include "interface.h"

#include "search.h"
#include "filter.h"

#include "dialog-filters.h"

gchar stmp_1[4096];
gchar stmp_2[4096];

GSList *searches = NULL;							/* List of search structs */

GtkWidget *default_search_clist    = NULL;	/* If no search are currently allocated */
GtkWidget *default_scrolled_window = NULL;	/* If no search are currently allocated */

struct search *current_search = NULL;			/*	The search currently displayed */

gboolean search_results_show_tabs = FALSE;	/* Do we have to display the notebook tabs */

void search_free_r_sets(struct search *);
void search_send_packet(struct search *);
void search_update_items(struct search *);

guint32 search_passive = 0;

/* --------------------------------------------------------------------------------------------------------- */

/* Row selected */

void on_clist_search_results_select_row(GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data)
{
	gtk_widget_set_sensitive(button_search_download, TRUE);
}

/* Row unselected */

void on_clist_search_results_unselect_row(GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data)
{
  gboolean sensitive;

  sensitive = current_search && GTK_CLIST(current_search->clist)->selection;
  gtk_widget_set_sensitive(button_search_download, sensitive);
}

/* Column title clicked */

void on_clist_search_results_click_column (GtkCList *clist, gint column, gpointer user_data)
{
	if(current_search == NULL)
	  return;

	/* Sorting by host doesn't work for now - so we disable it */

	if (column == 3) return;

	switch (column)
	{
		case 1:
			gtk_clist_set_compare_func(GTK_CLIST(current_search->clist), search_results_compare_size);
			break;

		case 2:
			gtk_clist_set_compare_func(GTK_CLIST(current_search->clist), search_results_compare_speed);
			break;

		case 3:
			gtk_clist_set_compare_func(GTK_CLIST(current_search->clist), search_results_compare_ip);
			break;

		default:
			gtk_clist_set_compare_func(GTK_CLIST(current_search->clist), NULL);
	}

	if (column == current_search->sort_col)
	{
		current_search->sort_order = (current_search->sort_order > 0)? -1 : 1;
	}
	else
	{
		current_search->sort_col = column;
		current_search->sort_order = 1;
	}

	gtk_clist_set_sort_type(GTK_CLIST(current_search->clist), (current_search->sort_order > 0)? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
	gtk_clist_set_sort_column(GTK_CLIST(current_search->clist), column);

	gtk_clist_sort(GTK_CLIST(current_search->clist));

	current_search->sort = TRUE;
}

/* Search results popup menu (glade puts funcs prototypes in callbacks.h) */

void on_popup_search_stop_sorting_activate (GtkMenuItem *menuitem, gpointer user_data)
{
	if (current_search) current_search->sort = FALSE;
}

void on_popup_search_filters_activate (GtkMenuItem *menuitem, gpointer user_data)
{
	filters_open_dialog();
}

void on_popup_search_close_activate (GtkMenuItem *menuitem, gpointer user_data)
{
	if (current_search) search_close_current();
}

void on_popup_search_toggle_tabs_activate (GtkMenuItem *menuitem, gpointer user_data)
{
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook_search_results), (search_results_show_tabs = !search_results_show_tabs));
}

void on_popup_search_restart_activate (GtkMenuItem *menuitem, gpointer user_data)
{
	if (current_search)
	{
		gtk_clist_clear(GTK_CLIST(current_search->clist));
		current_search->items = current_search->unseen_items = 0;
		search_send_packet(current_search);
		search_update_items(current_search);
	}
}

void on_popup_search_duplicate_activate (GtkMenuItem *menuitem, gpointer user_data)
{
	if (current_search) new_search(current_search->speed, current_search->query);
}

gboolean on_clist_search_results_button_press_event(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	if (event->button != 3) return FALSE;

	gtk_widget_set_sensitive(popup_search_toggle_tabs, (gboolean) searches);
	gtk_widget_set_sensitive(popup_search_close, (gboolean) searches);
	gtk_widget_set_sensitive(popup_search_restart, (gboolean) searches);
	gtk_widget_set_sensitive(popup_search_duplicate, (gboolean) searches);

	if (current_search)
	{
		gtk_clist_unselect_all(GTK_CLIST(current_search->clist));
		gtk_widget_set_sensitive(popup_search_stop_sorting, current_search->sort);
		g_snprintf(stmp_1, sizeof(stmp_1), "%s", current_search->query);
	}
	else
	{
		gtk_widget_set_sensitive(popup_search_stop_sorting, FALSE);
		g_snprintf(stmp_1, sizeof(stmp_1), "No current search");
	}

	gtk_label_set(GTK_LABEL((GTK_MENU_ITEM(popup_search_title)->item.bin.child)), stmp_1);

	g_snprintf(stmp_1, sizeof(stmp_1), (search_results_show_tabs)? "Hide tabs" : "Show tabs");

	gtk_label_set(GTK_LABEL((GTK_MENU_ITEM(popup_search_toggle_tabs)->item.bin.child)), stmp_1);

	gtk_menu_popup(GTK_MENU(popup_search), NULL, NULL, NULL, NULL, 3, 0);

	return TRUE;
}

/* Column resize */

void on_clist_search_results_resize_column(GtkCList *clist, gint column, gint width, gpointer user_data)
{
	static gboolean resizing = FALSE;
	GSList *l;

	if (resizing) return;

	resizing = TRUE;

	search_results_col_widths[column] = width;

	for (l = searches; l; l = l->next)
		gtk_clist_set_column_width(GTK_CLIST(((struct search *) l->data)->clist), column, width);

	resizing = FALSE;
}

/* Create a new GtkCList for search results */

void search_create_clist(GtkWidget **sw, GtkWidget **clist)
{
	GtkWidget *label;
	gint i;

	*sw = gtk_scrolled_window_new (NULL, NULL);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (*sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	*clist = gtk_clist_new (4);

	gtk_container_add (GTK_CONTAINER (*sw), *clist);
	for (i = 0; i < 4; i++) gtk_clist_set_column_width (GTK_CLIST (*clist), i, search_results_col_widths[i]);
	gtk_clist_set_selection_mode (GTK_CLIST (*clist), GTK_SELECTION_EXTENDED);
	gtk_clist_column_titles_show (GTK_CLIST (*clist));

	label = gtk_label_new ("File");
	gtk_clist_set_column_widget (GTK_CLIST (*clist), 0, label);

	label = gtk_label_new ("Size");
	gtk_clist_set_column_widget (GTK_CLIST (*clist), 1, label);

	label = gtk_label_new ("Speed");
	gtk_widget_show (label);
	gtk_clist_set_column_widget (GTK_CLIST (*clist), 2, label);

	label = gtk_label_new ("Host");
	gtk_clist_set_column_widget (GTK_CLIST (*clist), 3, label);
	
	gtk_widget_show_all (*sw);

	gtk_signal_connect(GTK_OBJECT(*clist), "select_row", GTK_SIGNAL_FUNC(on_clist_search_results_select_row), NULL);
	gtk_signal_connect(GTK_OBJECT(*clist), "unselect_row", GTK_SIGNAL_FUNC(on_clist_search_results_unselect_row), NULL);
	gtk_signal_connect(GTK_OBJECT(*clist), "click_column", GTK_SIGNAL_FUNC(on_clist_search_results_click_column), NULL);
	gtk_signal_connect(GTK_OBJECT(*clist), "button_press_event", GTK_SIGNAL_FUNC(on_clist_search_results_button_press_event), NULL);
	gtk_signal_connect(GTK_OBJECT(*clist), "resize-column", GTK_SIGNAL_FUNC(on_clist_search_results_resize_column), NULL);
}

void search_update_items(struct search *sch)
{
	if (sch && sch->items) g_snprintf(stmp_1, sizeof(stmp_1), "%u item%s found", sch->items, (sch->items > 1)? "s": "");
	else g_snprintf(stmp_1, sizeof(stmp_1), "No item found");
	gtk_label_set(GTK_LABEL(label_items_found), stmp_1);
}

struct search *search_selected = NULL;

void on_search_selected(GtkItem *i, gpointer data)
{
	search_selected = (struct search *) data;
}

gboolean updating = FALSE;

/* Like search_update_tab_label but always update the label */
void __search_update_tab_label(struct search *sch) {
  if(sch == current_search)
	g_snprintf(stmp_1, sizeof(stmp_1), "%s\n(%d)", sch->query, sch->items);
  else
	g_snprintf(stmp_1, sizeof(stmp_1), "%s\n(%d, %d)", sch->query, sch->items, sch->unseen_items);
  sch->last_update_items = sch->items;
  gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window, stmp_1);
  sch->last_update_time = time(NULL);
}

/* Doesn't update the label if nothing's changed or if the last update was
   recent. */
gboolean search_update_tab_label(struct search *sch)
{
  if(sch->items == sch->last_update_items)
	return TRUE;

  if(time(NULL) - sch->last_update_time < tab_update_time)
	return TRUE;

  __search_update_tab_label(sch);

  return TRUE;
}

void on_search_switch(struct search *sch)
{
	struct search *old_sch = current_search;
	g_return_if_fail(sch);

	current_search = sch;
	sch->unseen_items = 0; 

	if(old_sch)
	  __search_update_tab_label(old_sch);
	__search_update_tab_label(sch);

	search_update_items(sch);
	gui_update_minimum_speed(sch->speed);
	gtk_widget_set_sensitive(button_search_download, (gboolean) GTK_CLIST(sch->clist)->selection);

	if(sch->items == 0){
                gtk_widget_set_sensitive(button_search_clear, FALSE);
                gtk_widget_set_sensitive(popup_search_clear_results, FALSE);}
        else{
                gtk_widget_set_sensitive(button_search_clear, TRUE);
		gtk_widget_set_sensitive(popup_search_clear_results, TRUE);}

}

void on_search_popdown_switch(GtkWidget *w, gpointer data)
{
	struct search *sch = search_selected;
	if (!sch || updating) return;
	updating = TRUE;
	on_search_switch(sch);
	gtk_notebook_set_page(GTK_NOTEBOOK(notebook_search_results), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window));
	updating = FALSE;
}

void on_search_notebook_switch(GtkNotebook *notebook, GtkNotebookPage *page, gint page_num, gpointer user_data)
{
	struct search *sch = gtk_object_get_user_data((GtkObject *) page->child);
	g_return_if_fail(sch);
	if (updating) return;
	updating = TRUE;
	on_search_switch(sch);
	gtk_list_item_select(GTK_LIST_ITEM(sch->list_item));
	updating = FALSE;
}

/* */

void search_init(void)
{
	search_create_clist(&default_scrolled_window, &default_search_clist);
	gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_search_results), 0);
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook_search_results), TRUE);
	gtk_notebook_append_page(GTK_NOTEBOOK(notebook_search_results), default_scrolled_window, NULL);
	gtk_signal_connect(GTK_OBJECT(GTK_COMBO(combo_searches)->popwin), "hide", GTK_SIGNAL_FUNC(on_search_popdown_switch), NULL);
	gtk_signal_connect(GTK_OBJECT(notebook_search_results), "switch_page", GTK_SIGNAL_FUNC(on_search_notebook_switch), NULL);
	dialog_filters = create_dialog_filters();
	gtk_window_set_position(GTK_WINDOW(dialog_filters), GTK_WIN_POS_CENTER);
}

/* Free one results_set */

void search_free_r_set(struct results_set *rs)
{
	GSList *m;

	for (m = rs->records; m; m = m->next)
	{
		g_free(((struct record *) m->data)->name);
		g_free(m->data);
	}

	g_slist_free(rs->records);
	g_free(rs);
}

/* Free all the results_set's of a search */

void search_free_r_sets(struct search *sch)
{
	GSList *l;

	g_return_if_fail(sch);

	for (l = sch->r_sets; l; l = l->next)
		search_free_r_set((struct results_set *) l->data);

	g_slist_free(sch->r_sets);

	sch->r_sets = NULL;
}

/* Close a search */

void search_close_current(void)
{
	GList *glist;
	GSList *m;
	struct search *sch = current_search;

	if (sch->gh != NULL)	
		g_hook_destroy_link(&node_added_hook_list, sch->gh);

	sch->gh = NULL;

	g_return_if_fail(current_search);

	filters_close_search(sch);

	gtk_timeout_remove(sch->tab_updating);

	if(current_search->passive) {
	  if(!search_passive)
		g_error("search_close_current: This search is passive, but search_passive == 0.");
	  search_passive--;
	}

	searches = g_slist_remove(searches, (gpointer) sch);


	if (searches) /* Some other searches remain. */
	{
		gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_search_results), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window));
	}
	else	/* Keep the clist of this search, clear it and make it the default clist */
	{
		gtk_clist_clear(GTK_CLIST(sch->clist));

		default_search_clist = sch->clist;
		default_scrolled_window = sch->scrolled_window;

		search_selected = current_search = NULL;

		search_update_items(NULL);

		gtk_entry_set_text(GTK_ENTRY(combo_entry_searches), "");

		if (search_results_show_tabs) gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook_search_results), FALSE);

		gtk_widget_set_sensitive(button_search_clear, FALSE);
	        gtk_widget_set_sensitive(popup_search_clear_results, FALSE);

	}

	search_free_r_sets(sch);

	glist = g_list_prepend(NULL, (gpointer) sch->list_item);

	gtk_list_remove_items(GTK_LIST(GTK_COMBO(combo_searches)->list), glist);

	g_hash_table_destroy(sch->dups);

	for(m = sch->muids; m; m = m->next) {
	  g_free(m->data);
	}
	g_slist_free(sch->muids);

	g_free(sch->query);

	g_free(sch);

	gtk_widget_set_sensitive(combo_searches, (gboolean) searches);
	gtk_widget_set_sensitive(button_search_close, (gboolean) searches);
}

/* Create and send a search request packet */

void search_add_muid(struct search *sch, guchar *new_muid) {
  guchar *muid = (guchar *)g_malloc(16);

  memcpy(muid, new_muid, 16);
  sch->muids = g_slist_prepend(sch->muids, (gpointer)muid);
}

void __search_send_packet(struct search *sch, struct gnutella_node *n)
{
	struct  gnutella_msg_search *m;
	guint32 size;

	size = sizeof(struct gnutella_msg_search) + strlen(sch->query) + 1;

	m = (struct gnutella_msg_search *) g_malloc(size);

	message_set_muid(&(m->header));

	search_add_muid(sch, m->header.muid);

	m->header.function = GTA_MSG_SEARCH;
	m->header.ttl = my_ttl;
	m->header.hops = 0; /* (hops_random_factor)? (rand() % (hops_random_factor + 1)) : 0;*/

	WRITE_GUINT32_LE(size - sizeof(struct gnutella_header), m->header.size);

	WRITE_GUINT16_LE(minimum_speed, m->search.speed);

	strcpy(m->search.query, sch->query);

	message_add(m->header.muid, GTA_MSG_SEARCH, NULL);

	if (n)
	  sendto_one(n, (guchar*)m, NULL, size);
	else
	  sendto_all(   (guchar*)m, NULL, size);

	g_free(m);
}

void search_send_packet(struct search *sch) {
  __search_send_packet(sch, NULL);
}

guint search_hash_func(gconstpointer key) {
  struct record *rc = (struct record *)key;
  return 
	g_str_hash(rc->name) ^
	g_int_hash(&rc->size) ^
	g_int_hash(&rc->index) ^
	g_int_hash(&rc->results_set->ip) ^
	g_int_hash(&rc->results_set->speed) ^
	g_int_hash(&rc->results_set->port);
}

gint search_hash_key_compare(gconstpointer a, gconstpointer b) {
  struct record *this_record = (struct record *)a;
  struct record *rc = (struct record *)b;
  return
	   !strcmp(rc->name, this_record->name)
	&& !memcmp(rc->results_set->guid, this_record->results_set->guid, 16)
	&& rc->index == this_record->index
	&& rc->size == this_record->size
	&& rc->results_set->ip == this_record->results_set->ip
	&& rc->results_set->port == this_record->results_set->port
	;
}


/* Start a new search */

struct search *new_search(guint16 speed, gchar *query) {
  return _new_search(speed, query, TRUE);
}

static void node_added_callback(gpointer data) {
  g_assert(node_added);
  g_assert(data);
  __search_send_packet((struct search *)data, node_added);
}

struct search *_new_search(guint16 speed, gchar *query, gboolean send_packet)
{
	struct  search *sch;
	GList   *glist;

	sch = (struct search *) g_malloc0(sizeof(struct search));

	sch->query = g_strdup(query);
	sch->speed = minimum_speed;

	if(send_packet)
	  search_send_packet(sch);

	/* Create the list item */

	sch->list_item = gtk_list_item_new_with_label(sch->query);

	gtk_widget_show(sch->list_item);

	glist = g_list_prepend(NULL, (gpointer) sch->list_item);

	gtk_list_prepend_items(GTK_LIST(GTK_COMBO(combo_searches)->list), glist);

	/* Create a new CList if needed, or use the default CList */

	if (searches)	/* We have to create a new clist for this search */
	{
		search_create_clist(&sch->scrolled_window, &sch->clist);

		gtk_object_set_user_data((GtkObject *) sch->scrolled_window, (gpointer) sch);

		gtk_notebook_append_page(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window, NULL);

		gtk_notebook_set_page(GTK_NOTEBOOK(notebook_search_results), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window));
	}
	else 				/* There are no searches currently, we can use the default clist */
	{
		if (default_scrolled_window && default_search_clist)
		{
			sch->scrolled_window = default_scrolled_window;
			sch->clist = default_search_clist;

			default_search_clist = default_scrolled_window = NULL;
		}
		else g_warning("new_search(): No current search but no default clist !?\n");

		gtk_object_set_user_data((GtkObject *) sch->scrolled_window, (gpointer) sch);
	}

	search_update_tab_label(sch);
	sch->tab_updating = gtk_timeout_add(tab_update_time * 1000,
										(GtkFunction)search_update_tab_label,
										sch);

	sch->dups = g_hash_table_new(search_hash_func, search_hash_key_compare);
	if(!sch->dups)
	  g_error("new_search: unable to allocate hash table.\n");

	if (!searches && search_results_show_tabs) gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook_search_results), TRUE);

	gtk_signal_connect(GTK_OBJECT(sch->list_item), "select", GTK_SIGNAL_FUNC(on_search_selected), (gpointer) sch);

	on_search_switch(sch);

	gtk_widget_set_sensitive(combo_searches, TRUE);
	gtk_widget_set_sensitive(button_search_close, TRUE);

	gtk_entry_set_text(GTK_ENTRY(entry_search), "");

	searches = g_slist_append(searches, (gpointer) sch);

	filters_new_search(sch);

	if(send_packet) {
	  sch->gh = g_hook_alloc(&node_added_hook_list);

	  sch->gh->data = sch;
	  sch->gh->func = node_added_callback;

	  g_hook_prepend(&node_added_hook_list, sch->gh);
	}

	return sch;
}

/* Searches results */

gint search_compare(gint sort_col, struct record *r1, struct record *r2)
{
	switch (sort_col)
	{
		case 0: return strcmp(r1->name, r2->name);
		case 1: return r1->size - r2->size;
		case 2: return r1->results_set->speed - r2->results_set->speed;
		case 3: return r1->results_set->ip - r2->results_set->ip;
	}
	return 0;
}

struct results_set *get_results_set(struct gnutella_node *n) {
	struct results_set *rs;
	struct record *rc;
	gchar *e, *s, *fname;
	guint32 nr, size, index;
	struct gnutella_search_results *r;

	rs = (struct results_set *) g_malloc0(sizeof(struct results_set));

	r = (struct gnutella_search_results *) n->data;

	rs->num_recs = (guint8) r->num_recs;
	READ_GUINT32_BE(r->host_ip, rs->ip);
	READ_GUINT16_LE(r->host_port, rs->port);
	READ_GUINT32_LE(r->host_speed, rs->speed);

	s  = r->records;					/* Start of the records */
	e  = s + n->size - 16 - 11;	/* End of the records */
	nr = 0;

	while (s < e && nr < rs->num_recs)
	{
		READ_GUINT32_LE(s, index); s += 4;
		READ_GUINT32_LE(s, size);  s += 4;
		fname = s;

		while (s < e && *s) s++;

		if (s >= e)
		{
			fprintf(stderr, "Node %s: %u records found in set (node said %u records)\n", node_ip(n), nr, rs->num_recs);
			break;
		}

		if (s[1])
		{
			fprintf(stderr, "Node %s: Record %u is not double-NULL terminated (%c (0x%x) instead)!\n", node_ip(n), nr, (char)s[1], (int)(char)s[1]);
			if(isalnum(s[1]))
			   s--; /* allow for s += 2, assuming s[1] the beginning of the next entry. */
		}

		/* Okay, one more record */

		nr++;

		rc = (struct record *) g_malloc0(sizeof(struct record));

		rc->index = index;
		rc->size  = size;
		rc->name  = g_strdup(fname);

		rc->results_set = rs;

		rs->records = g_slist_prepend(rs->records, (gpointer) rc);

		s += 2;	/* Skip the two null bytes at the end */
	}

	if (s < e)
	{
		fprintf(stderr, "Node %s: %u records found in set, but %u bytes remains after the records !\n", node_ip(n), nr, e - s);
	}
	else if (s > e)
	{
		fprintf(stderr, "Node %s: %u records found in set, but last record exceeded the struct by %u bytes !\n", node_ip(n), nr, s - e);
	}

	/* We now have the guid of the node */

	memcpy(rs->guid, s, 16);

	return rs;
}

gboolean search_result_is_dup(struct search *sch, struct record *rc) {
  return (gboolean)g_hash_table_lookup(sch->dups, rc);
}

void search_matched(struct search *sch, struct results_set *rs) {
	GSList *l, *next;
	gchar *titles[4];
	struct record *rc;
	guint32 row;

	sch->r_sets = g_slist_prepend(sch->r_sets, (gpointer) rs);	/* Adds the set to the list */

	/* Update the GUI */

	gtk_clist_freeze(GTK_CLIST(sch->clist));

	for (l = rs->records; l; l = next)
	{
		next = l->next;

		rc = (struct record *) l->data;

		if (search_result_is_dup(sch, rc) || !filter_record(sch, rc)) {
		  rs->records = g_slist_remove_link(rs->records, l);
		  rs->num_recs--;
		  continue;
		}

		g_hash_table_insert(sch->dups, rc, (void*) 1);

		titles[0] = rc->name;
		titles[1] = short_size(rc->size);
		g_snprintf(stmp_2, sizeof(stmp_2), "%u", rs->speed); titles[2] = stmp_2;
		titles[3] = ip_port_to_gchar(rs->ip, rs->port);

		if (!sch->sort) row = gtk_clist_append(GTK_CLIST(sch->clist), titles);
		else
		{
			/* gtk_clist_set_auto_sort() can't work for row data based sorts ! Too bad. */
			/* So we need to find the place to put the result by ourselves. */

			GList *work;
	  
			row = 0;

			work = GTK_CLIST(sch->clist)->row_list;

			if (sch->sort_order > 0)
			{
				while (row < GTK_CLIST(sch->clist)->rows && 
					   search_compare(sch->sort_col, rc, (struct record *) GTK_CLIST_ROW(work)->data) > 0)
				{
					row++;
					work = work->next;
				}
			}
			else
			{
				while (row < GTK_CLIST(sch->clist)->rows && 
					   search_compare(sch->sort_col, rc, (struct record *) GTK_CLIST_ROW(work)->data) < 0)
				{
					row++;
					work = work->next;
				}
			}

			gtk_clist_insert(GTK_CLIST(sch->clist), row, titles);
		}

		gtk_clist_set_row_data(GTK_CLIST(sch->clist), row, (gpointer) rc);
	}

	gtk_clist_thaw(GTK_CLIST(sch->clist));

	if((sch->items == 0) && (sch == current_search)){
                gtk_widget_set_sensitive(button_search_clear, TRUE);
	        gtk_widget_set_sensitive(popup_search_clear_results, TRUE);}

	if (sch == current_search) {
	  sch->items += rs->num_recs;
	  search_update_items(sch);
	} else {
	  sch->items += rs->num_recs;
	  sch->unseen_items += rs->num_recs;
	}

	if(time(NULL) - sch->last_update_time < tab_update_time)
	  search_update_tab_label(sch);
}

gboolean search_has_muid(struct search *sch, guchar *muid) {
  GSList *m;

  for(m = sch->muids; m; m = m->next)
	if (!memcmp(muid, (guchar*)m->data, 16))
	  return TRUE;
  return FALSE;
}

void search_results(struct gnutella_node *n)
{
	struct search *sch;
	struct results_set *rs;
	GSList *l;

	/* Find the search matching the MUID */

	for (l = searches; l; l = l->next)
	{
		sch = (struct search *) l->data;
		if (sch->passive || search_has_muid(sch, n->header.muid)) {
			/* This should eventually be replaced with some sort of
               copy_results_set so that we don't have to repeatedly parse
               the packet. */
			rs = get_results_set(n);

			if(rs == NULL) {
				g_warning("search_results: get_results_set returned NULL.\n");
				return;
			}

			/* The result set is ok */
			search_matched(sch, rs);
		}
	}
}


void search_clear_results(void) {
  gtk_clist_clear(GTK_CLIST(current_search->clist));
  current_search->items = current_search->unseen_items = 0;
  __search_update_tab_label(current_search);
}

void
on_button_search_clear_clicked (GtkButton *button, gpointer user_data) {
  search_clear_results();

  gtk_widget_set_sensitive(button_search_clear, FALSE);
  gtk_widget_set_sensitive(popup_search_clear_results, FALSE);

}


void
on_popup_search_clear_results_activate (GtkMenuItem *menuitem, gpointer user_data) {
  search_clear_results();

  gtk_widget_set_sensitive(button_search_clear, FALSE);
  gtk_widget_set_sensitive(popup_search_clear_results, FALSE);

}




/* ------------------------------------------------------------------------------------------------ */


void download_selection_of_clist(GtkCList *c) {
	struct results_set *rs;
	struct record *rc;
	GList *l;

	for (l = c->selection; l; l = l->next) {
		rc = (struct record *) gtk_clist_get_row_data(c, (gint) l->data);
		rs = rc->results_set;
		download_new(rc->name, rc->size, rc->index, rs->ip, rs->port, rs->guid);
	}
}


void search_download_files(void)
{
	/* Download the selected files */

    if(jump_to_downloads) {
	  gtk_notebook_set_page(GTK_NOTEBOOK(notebook_main), 2);
	  gtk_clist_select_row(GTK_CLIST(clist_menu), 2, 0);
	}

	if (current_search) {
		download_selection_of_clist(GTK_CLIST(current_search->clist));
		gtk_clist_unselect_all(GTK_CLIST(current_search->clist));
	} else {
	  g_warning("search_download_files(): no possible search!\n");
	}
}

/* ------------------------------------------------------------------------------------------------ */

gint search_results_compare_size(GtkCList *clist, gconstpointer ptr1, gconstpointer ptr2)
{
	return (((struct record *) ((GtkCListRow *) ptr1)->data)->size - ((struct record *) ((GtkCListRow *) ptr2)->data)->size);
}

gint search_results_compare_speed(GtkCList *clist, gconstpointer ptr1, gconstpointer ptr2)
{
	return (((struct record *) ((GtkCListRow *) ptr1)->data)->results_set->speed - ((struct record *) ((GtkCListRow *) ptr2)->data)->results_set->speed);
}

gint search_results_compare_ip(GtkCList *clist, gconstpointer ptr1, gconstpointer ptr2)
{
	return (((struct record *) ((GtkCListRow *) ptr1)->data)->results_set->ip - ((struct record *) ((GtkCListRow *) ptr2)->data)->results_set->ip);
}

/* vi: set ts=3: */


