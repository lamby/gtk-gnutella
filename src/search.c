
/* Handle searches */

#include "gnutella.h"

#include "interface.h"

gchar stmp_1[4096];
gchar stmp_2[4096];

struct results_set
{
	guchar guid[16];
	guint32 num_recs;
	guint32 ip;
	guint16 port;
	guint32 speed;

	GSList *records;
};

struct record
{
	struct results_set *results_set;
	gchar *name;
	guint32 size;
	guint32 index;
};

struct search
{
	GtkWidget *clist;						/* GtkCList for this search */
	GtkWidget *scrolled_window;		/* GtkScrolledWindow containing the GtkCList */
	gchar 	*query;						/* The search query */
	guint16	speed;						/* Minimum speed for the results of this query */
	time_t	time;							/* Time when this search was started */
	guchar	muid[16];					/* Message UID of this search */
	GSList	*r_sets;						/* The results sets of this search */
	guint32	items;						/* Total number of items for this search */
	guint32  displayed;					/* Total number of items displayed */

	gint sort_col;							/* Column to sort */
	gint sort_order;						/* Ascending or descending */
	gboolean sort;							/* Do sorting or not */

	// XXX Other fields will be needed for the advanced filtering
};

GSList *searches = NULL;							/* List of search structs */

GtkWidget *default_search_clist    = NULL;	/* If no search are currently allocated */
GtkWidget *default_scrolled_window = NULL;	/* If no search are currently allocated */

struct search *current_search = NULL;			/*	The search currently displayed */

/* GUI Part ------------------------------------------------------------------------------------------------ */

/* Creates a new GtkCList for search results */

void on_clist_search_results_select_row(GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data)
{
	gtk_widget_set_sensitive(button_search_download, TRUE);
}

void on_clist_search_results_unselect_row(GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data)
{
	gtk_widget_set_sensitive(button_search_download, (gboolean) clist->selection);
}

void search_create_clist(GtkWidget **sw, GtkWidget **clist)
{
	GtkWidget *label;

	*sw = gtk_scrolled_window_new (NULL, NULL);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (*sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	*clist = gtk_clist_new (4);

	gtk_container_add (GTK_CONTAINER (*sw), *clist);
	gtk_clist_set_column_width (GTK_CLIST (*clist), 0, 290);
	gtk_clist_set_column_width (GTK_CLIST (*clist), 1, 80);
	gtk_clist_set_column_width (GTK_CLIST (*clist), 2, 50);
	gtk_clist_set_column_width (GTK_CLIST (*clist), 3, 140);
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
}

void search_update_items(struct search *sch)
{
	if (sch && sch->items) g_snprintf(stmp_1, sizeof(stmp_1), "%u item%s found", sch->items, (sch->items > 1)? "s": "");
	else g_snprintf(stmp_1, sizeof(stmp_1), "No items found");
	gtk_label_set(GTK_LABEL(label_items_found), stmp_1);
}

void on_search_switch(GtkMenuItem *item, gpointer user_data)
{
	struct search *sch = (struct search *) user_data;

	g_return_if_fail(sch);

	current_search = sch;

	gtk_notebook_set_page(GTK_NOTEBOOK(notebook_search_results), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window));

	search_update_items(sch);

	gui_update_minimum_speed(sch->speed);

	gtk_widget_set_sensitive(button_search_download, (gboolean) GTK_CLIST(sch->clist)->selection);
}

void search_rebuild_option_menu(void)
{
	GtkWidget *menuitem;

	gtk_option_menu_remove_menu(GTK_OPTION_MENU(option_menu_search));

	/* Why the next call always fails with warning "assertion `GTK_IS_WIDGET (widget)' failed." ? */
	/* gtk_widget_destroy(option_menu_search_menu); */
	
	option_menu_search_menu = gtk_menu_new();

	if (searches)
	{
		GSList *l;

		for (l = searches; l; l = l->next)
		{
			menuitem = gtk_menu_item_new_with_label(((struct search *) l->data)->query);
			gtk_widget_show(menuitem);
			gtk_menu_prepend(GTK_MENU(option_menu_search_menu), menuitem);
			gtk_signal_connect(GTK_OBJECT(menuitem), "activate", GTK_SIGNAL_FUNC(on_search_switch), (gpointer) (struct search *) l->data);
		}
	}
	else
	{
		menuitem = gtk_menu_item_new_with_label("No searches");
		gtk_widget_show(menuitem);
		gtk_menu_append(GTK_MENU(option_menu_search_menu), menuitem);
	}

	gtk_widget_set_sensitive(option_menu_search, (gboolean) searches);
	gtk_widget_set_sensitive(button_search_filter, (gboolean) searches);
	gtk_widget_set_sensitive(button_search_close, (gboolean) searches);

	gtk_option_menu_set_menu(GTK_OPTION_MENU(option_menu_search), option_menu_search_menu);
}

/* */

void search_init(void)
{
	search_create_clist(&default_scrolled_window, &default_search_clist);

	gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_search_results), 0);

	gtk_notebook_append_page(GTK_NOTEBOOK(notebook_search_results), default_scrolled_window, NULL);
}

/* Free all the results sets of a search */

void search_free_r_sets(struct search *sch)
{
	GSList *l, *m;

	g_return_if_fail(sch);

	for (l = sch->r_sets; l; l = l->next)
	{
		for (m = ((struct results_set *) l->data)->records; m; m = m->next)
		{
			g_free(((struct record *) m->data)->name);
			g_free(m->data);
		}

		g_slist_free(((struct results_set *) l->data)->records);

		g_free(l->data);
	}

	g_slist_free(sch->r_sets);
}

/* Closes a search */

void search_close_current(void)
{
	struct search *sch = current_search;

	g_return_if_fail(current_search);

	searches = g_slist_remove(searches, (gpointer) sch);

	search_free_r_sets(sch);

	if (searches)
	{
		/* Some others searches remains. We destroy the clist of this search and display another search */

		gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_search_results), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_search_results), sch->scrolled_window));

		/* Why the next call always fails with warning "assertion `GTK_IS_WIDGET (widget)' failed." ? */
		/* gtk_widget_destroy(sch->scrolled_window); */

		on_search_switch( NULL, searches->data);	/* Switch to the first search in the list */
	}
	else		/* Keep the clist of this search, clear it and make it the default clist */
	{
		gtk_clist_clear(GTK_CLIST(sch->clist));

		default_search_clist = sch->clist;
		default_scrolled_window = sch->scrolled_window;

		current_search = NULL;

		search_update_items(NULL);
	}

	g_free(sch->query);

	g_free(sch);

	search_rebuild_option_menu();
}

/* Starts a new search */

void new_search(guint16 speed, gchar *query)
{
	struct  gnutella_msg_search *m;
	struct  search *sch;
	guint32 size;

	sch = (struct search *) g_malloc0(sizeof(struct search));

	sch->query = g_strdup(query);
	sch->speed = minimum_speed;

	size = sizeof(struct gnutella_msg_search) + strlen(query) + 1;

	m = (struct gnutella_msg_search *) g_malloc(size);

	message_set_muid(&(m->header));

	memcpy(sch->muid, m->header.muid, 16);

	m->header.function = GTA_MSG_SEARCH;
	m->header.ttl = my_ttl;
	m->header.hops = 0;

	WRITE_GUINT32_LE(size - sizeof(struct gnutella_header), m->header.size);

	WRITE_GUINT16_LE(minimum_speed, m->search.speed);

	strcpy(m->search.query, query);

	message_add(m->header.muid, GTA_MSG_SEARCH, NULL);

	sendto_all((guchar *) m, NULL, size);

	g_free(m);

	if (searches)	/* We have to create a new clist for this search */
	{
		search_create_clist(&sch->scrolled_window, &sch->clist);

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
	}

	searches = g_slist_append(searches, (gpointer) sch);

	current_search = sch;

	search_update_items(current_search);

	search_rebuild_option_menu();

	gtk_entry_set_text(GTK_ENTRY(entry_search), "");
}

/* Searches results */

gint search_compare(struct record *r1, struct record *r2)
{
	switch (search_results_sort_col)
	{
		case 0: return strcmp(r1->name, r2->name);
		case 1: return r1->size - r2->size;
		case 2: return r1->results_set->speed - r2->results_set->speed;
		case 3: return r1->results_set->ip - r2->results_set->ip;
	}
	return 0;
}

void search_results(struct gnutella_node *n)
{
	struct search *sch;
	struct gnutella_search_results *r;
	struct results_set *rs;
	struct record *rc;
	gchar *e, *s, *fname;
	guint32 row, nr, size, index;
	GSList *l;
	gchar *titles[4];

	/* Finds the search matching the MUID */

	for (l = searches; l; l = l->next)
	{
		if (!memcmp(n->header.muid, ((struct search *) l->data)->muid, 16)) break;
	}

	if (!l) return;	/* This search has been closed */

	sch = (struct search *) l->data;

	r = (struct gnutella_search_results *) n->data;

	rs = (struct results_set *) g_malloc0(sizeof(struct results_set));

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
/*			fprintf(stderr, "Node %s: %u records found in set (node said %u records)\n", node_ip(n), nr, rs->num_recs); */
			g_free(rs);
			return;
		}

		if (s[1])
		{
/*			fprintf(stderr, "Node %s: Record %u is not double-NULL terminated !\n", node_ip(n), nr); */
			g_free(rs);
			return;
		}

		/* Okay, one more record */

		nr++;

		rc = (struct record *) g_malloc0(sizeof(struct record));

		rc->index = index;
		rc->size  = size;
		rc->name  = g_strdup(fname);

		rc->results_set = rs;

		rs->records = g_slist_prepend(rs->records, (gpointer) rc);

		s += 2;	/* Skips the two null bytes at the end */
	}

	if (s < e)
	{
/*		fprintf(stderr, "Node %s: %u records found in set, but %u bytes remains after the records !\n", node_ip(n), nr, e - s); */
		/* TODO FREE ALL THE RECORDS OF THE SET */
		g_free(rs);
		return;
	}
	else if (s > e)
	{
/*		fprintf(stderr, "Node %s: %u records found in set, but last record exceeded the struct by %u bytes !\n", node_ip(n), nr, s - e); */
		/* TODO FREE ALL THE RECORDS OF THE SET */
		g_free(rs);
		return;
	}

	/* We now have the guid of the node */

	memcpy(rs->guid, s, 16);

	/* The result set is ok */

	sch->r_sets = g_slist_prepend(sch->r_sets, (gpointer) rs);	/* Adds the set to the list */

	/* Update the GUI */

	gtk_clist_freeze(GTK_CLIST(sch->clist));

	for (l = rs->records; l; l = l->next)
	{
		rc = (struct record *) l->data;

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
				while (row < GTK_CLIST(sch->clist)->rows && search_compare(rc, (struct record *) GTK_CLIST_ROW(work)->data) > 0)
				{
					row++;
					work = work->next;
				}
			}
			else
			{
				while (row < GTK_CLIST(sch->clist)->rows && search_compare(rc, (struct record *) GTK_CLIST_ROW(work)->data) < 0)
				{
					row++;
					work = work->next;
				}
			}

			gtk_clist_insert(GTK_CLIST(sch->clist), row, titles);
		}

		gtk_clist_set_row_data(GTK_CLIST(sch->clist), row, (gpointer) rc);

		sch->items++;
	}

	gtk_clist_thaw(GTK_CLIST(sch->clist));

	if (sch == current_search) search_update_items(sch);
}

/* ------------------------------------------------------------------------------------------------ */

void search_download_files(void)
{
	/* Download the selected files */

	struct results_set *rs;
	struct record *rc;
	GList *l;

	gtk_notebook_set_page(GTK_NOTEBOOK(notebook_main), 2);
	gtk_clist_select_row(GTK_CLIST(clist_menu), 2, 0);

	if (current_search)
	{
		for (l = GTK_CLIST(current_search->clist)->selection; l; l = l->next)
		{
			rc = (struct record *) gtk_clist_get_row_data(GTK_CLIST(current_search->clist), (gint) l->data);
			rs = rc->results_set;
			download_new(rc->name, rc->size, rc->index, rs->ip, rs->port, rs->guid);
		}

		gtk_clist_unselect_all(GTK_CLIST(current_search->clist));
	}
	else g_warning("search_download_files(): Current search is NULL !\n");
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

