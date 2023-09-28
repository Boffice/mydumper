/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    Domas Mituzas, Facebook ( domas at fb dot com )
                    Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
                    Andrew Hutchings, MariaDB Foundation (andrew at mariadb dot org)
                    Max Bubenick, Percona RDBA (max dot bubenick at percona dot com)
                    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <mysql.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <math.h>
#include "common.h"
#include "mydumper_start_dump.h"
#include "server_detect.h"
#include "mydumper_chunks.h"
#include "mydumper_database.h"
#include "mydumper_jobs.h"
#include "mydumper_global.h"
#include "regex.h"
#include "mydumper_working_thread.h"
#include "mydumper_write.h"
#include "mydumper_integer_chunks.h"
#include "mydumper_common.h"

guint64 gint64_abs(gint64 a){
  if (a >= 0)
    return a;
  return -a;
}

void initialize_integer_step(union chunk_step *cs, gboolean is_unsigned, union type type, gboolean is_step_fixed_length, guint64 step, guint64 min_css, guint64 max_css, gboolean check_min, gboolean check_max){
  cs->integer_step.is_unsigned = is_unsigned;
  cs->integer_step.step = step;
  cs->integer_step.min_chunk_step_size = min_css;
  cs->integer_step.max_chunk_step_size = max_css;
  if (cs->integer_step.is_unsigned){
    cs->integer_step.type.unsign.min = type.unsign.min;
    cs->integer_step.type.unsign.cursor = cs->integer_step.type.unsign.min;
    cs->integer_step.type.unsign.max = type.unsign.max;
    cs->integer_step.estimated_remaining_steps=(cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.min) / cs->integer_step.step;
  }else{
    cs->integer_step.type.sign.min = type.sign.min;
    cs->integer_step.type.sign.cursor = cs->integer_step.type.sign.min;
    cs->integer_step.type.sign.max = type.sign.max;
    cs->integer_step.estimated_remaining_steps=(cs->integer_step.type.sign.max - cs->integer_step.type.sign.min) / cs->integer_step.step;
  }
  cs->integer_step.is_step_fixed_length = is_step_fixed_length;
  cs->integer_step.check_max=check_max;
  cs->integer_step.check_min=check_min;
}

union chunk_step *new_integer_step(gboolean is_unsigned, union type type, gboolean is_step_fixed_length, guint64 step, guint64 min_css, guint64 max_css, gboolean check_min, gboolean check_max){
  union chunk_step * cs = g_new0(union chunk_step, 1);
  initialize_integer_step(cs,is_unsigned, type, is_step_fixed_length, step, min_css, max_css, check_min, check_max);
  return cs;
}

void initialize_integer_step_item(struct chunk_step_item *csi, gboolean include_null, GString *prefix, gchar *field, gboolean is_unsigned, union type type, guint deep, gboolean is_step_fixed_length, guint64 step, guint64 min_css, guint64 max_css, guint64 number, gboolean check_min, gboolean check_max, struct chunk_step_item * next, guint position){
  csi->chunk_step = new_integer_step(is_unsigned, type, is_step_fixed_length, step, min_css, max_css, check_min, check_max);
  csi->chunk_type=INTEGER;
  csi->position=position;
  csi->next=next;
  csi->status = UNASSIGNED;
  csi->chunk_functions.process = &process_integer_chunk;
//  csi->chunk_functions.update_where = &get_integer_chunk_where;
  csi->chunk_functions.get_next = &get_next_integer_chunk;
  csi->where=g_string_new("");
  csi->include_null = include_null;
  csi->prefix = prefix;
  csi->field = g_strdup(field);
  csi->mutex = g_mutex_new();
  csi->number = number;
  csi->deep = deep;
}

struct chunk_step_item *new_integer_step_item(gboolean include_null, GString *prefix, gchar *field, gboolean is_unsigned, union type type, guint deep, gboolean is_step_fixed_length, guint64 step, guint64 min_css, guint64 max_css, guint64 number, gboolean check_min, gboolean check_max, struct chunk_step_item * next, guint position){
  struct chunk_step_item *csi = g_new0(struct chunk_step_item,1);
  initialize_integer_step_item(csi, include_null, prefix, field, is_unsigned, type, deep, is_step_fixed_length, step, min_css, max_css, number, check_min, check_max, next, position);
  return csi;
}



void free_integer_step(union chunk_step * cs){
  g_free(cs);
}

void free_integer_step_item(struct chunk_step_item * csi){
  free_integer_step(csi->chunk_step);
//  g_free(csi);
}

void common_to_chunk_step(struct db_table *dbt, struct chunk_step_item * csi, struct chunk_step_item * new_csi){
  csi->deep=csi->deep+1;
  new_csi->status=ASSIGNED;

  dbt->chunks=g_list_append(dbt->chunks,new_csi);
  g_async_queue_push(dbt->chunks_queue, csi);
  g_async_queue_push(dbt->chunks_queue, new_csi);
}

guint64 my_pow_two(guint max){
  guint64 r=1;
  guint i=0; 
  for (i=1;i<max;i++){
    r*=2;
  }
  return r;
}

struct chunk_step_item * split_unsigned_chunk_step(struct db_table *dbt, struct chunk_step_item * csi){
  guint64 new_minmax = 0;
  union type type;
  type.unsign.max = csi->chunk_step->integer_step.type.unsign.max;
  struct chunk_step_item * new_csi = NULL;

  if ( csi->chunk_step->integer_step.is_step_fixed_length ){
    if ( csi->status == DUMPING_CHUNK ){
      new_minmax = csi->chunk_step->integer_step.type.unsign.cursor + csi->chunk_step->integer_step.step *
                (( csi->chunk_step->integer_step.type.unsign.max    / csi->chunk_step->integer_step.step - 
                   csi->chunk_step->integer_step.type.unsign.cursor / csi->chunk_step->integer_step.step ) / 2 ) + 1;

      if (new_minmax == csi->chunk_step->integer_step.type.unsign.cursor)
      new_minmax = csi->chunk_step->integer_step.type.unsign.cursor + csi->chunk_step->integer_step.step * (
                 ( csi->chunk_step->integer_step.type.unsign.max    / csi->chunk_step->integer_step.step -
                   csi->chunk_step->integer_step.type.unsign.cursor / csi->chunk_step->integer_step.step ) / 2  + 1) + 1;

    }else{
      new_minmax = csi->chunk_step->integer_step.type.unsign.min    + csi->chunk_step->integer_step.step *
                (( csi->chunk_step->integer_step.type.unsign.max    / csi->chunk_step->integer_step.step - 
                   csi->chunk_step->integer_step.type.unsign.min    / csi->chunk_step->integer_step.step ) / 2 );

      if (new_minmax == csi->chunk_step->integer_step.type.unsign.min)
      new_minmax = csi->chunk_step->integer_step.type.unsign.min    + csi->chunk_step->integer_step.step * (
                 ( csi->chunk_step->integer_step.type.unsign.max    / csi->chunk_step->integer_step.step -
                   csi->chunk_step->integer_step.type.unsign.min    / csi->chunk_step->integer_step.step ) / 2 + 1);


    }
    type.unsign.min = new_minmax;
    new_csi = new_integer_step_item(FALSE, NULL, dbt->primary_key->data, csi->chunk_step->integer_step.is_unsigned, type, csi->deep + 1, csi->chunk_step->integer_step.is_step_fixed_length, csi->chunk_step->integer_step.step, csi->chunk_step->integer_step.min_chunk_step_size, csi->chunk_step->integer_step.max_chunk_step_size, csi->number, TRUE, csi->chunk_step->integer_step.check_max, NULL, csi->position);
  }else{
    if ( csi->status == DUMPING_CHUNK ){
      new_minmax = csi->chunk_step->integer_step.type.unsign.cursor + (csi->chunk_step->integer_step.type.unsign.max - csi->chunk_step->integer_step.type.unsign.cursor)/2;
      if ( new_minmax == csi->chunk_step->integer_step.type.unsign.cursor )
        new_minmax++;
    }else{
      new_minmax = csi->chunk_step->integer_step.type.unsign.min    + (csi->chunk_step->integer_step.type.unsign.max - csi->chunk_step->integer_step.type.unsign.min)/2;
      if ( new_minmax == csi->chunk_step->integer_step.type.unsign.min )
        new_minmax++;
    }
    type.unsign.min = new_minmax;
    new_csi = new_integer_step_item(FALSE, NULL, dbt->primary_key->data, csi->chunk_step->integer_step.is_unsigned, type, csi->deep + 1, csi->chunk_step->integer_step.is_step_fixed_length, csi->chunk_step->integer_step.step, csi->chunk_step->integer_step.min_chunk_step_size, csi->chunk_step->integer_step.max_chunk_step_size, csi->number+pow(2,csi->deep), TRUE, csi->chunk_step->integer_step.check_max, NULL, csi->position);
    csi->chunk_step->integer_step.check_max=TRUE;
    
  }

  csi->chunk_step->integer_step.type.unsign.max = new_minmax - 1;

  common_to_chunk_step(dbt, csi, new_csi);

  return new_csi;
}

struct chunk_step_item * split_signed_chunk_step(struct db_table *dbt, struct chunk_step_item * csi){
  gint64 new_minmax = 0;
  union type type;

  type.sign.max = csi->chunk_step->integer_step.type.sign.max;

  struct chunk_step_item * new_csi = NULL;
  if ( csi->chunk_step->integer_step.is_step_fixed_length ){
    if ( csi->status == DUMPING_CHUNK ){
       new_minmax = csi->chunk_step->integer_step.type.sign.cursor + (signed) csi->chunk_step->integer_step.step *
                 (( csi->chunk_step->integer_step.type.sign.max    / (signed) csi->chunk_step->integer_step.step - 
                    csi->chunk_step->integer_step.type.sign.cursor / (signed) csi->chunk_step->integer_step.step ) / 2 ) + 1;
      if (new_minmax == csi->chunk_step->integer_step.type.sign.min)
       new_minmax = csi->chunk_step->integer_step.type.sign.min    + (signed) csi->chunk_step->integer_step.step *
                 (( csi->chunk_step->integer_step.type.sign.max    / (signed) csi->chunk_step->integer_step.step -
                    csi->chunk_step->integer_step.type.sign.min    / (signed) csi->chunk_step->integer_step.step ) / 2 + 1 ) + 1; 
   }else{
       new_minmax = csi->chunk_step->integer_step.type.sign.min    + (signed) csi->chunk_step->integer_step.step *
                 (( csi->chunk_step->integer_step.type.sign.max    / (signed) csi->chunk_step->integer_step.step - 
                    csi->chunk_step->integer_step.type.sign.min    / (signed) csi->chunk_step->integer_step.step ) / 2 );
      if (new_minmax == csi->chunk_step->integer_step.type.sign.min)
       new_minmax = csi->chunk_step->integer_step.type.sign.min    + (signed) csi->chunk_step->integer_step.step *
                 (( csi->chunk_step->integer_step.type.sign.max    / (signed) csi->chunk_step->integer_step.step -
                    csi->chunk_step->integer_step.type.sign.min    / (signed) csi->chunk_step->integer_step.step ) / 2 + 1);
    }
    type.sign.min = new_minmax;

    new_csi = new_integer_step_item(FALSE, NULL, dbt->primary_key->data, csi->chunk_step->integer_step.is_unsigned, type, csi->deep + 1, csi->chunk_step->integer_step.is_step_fixed_length, csi->chunk_step->integer_step.step, csi->chunk_step->integer_step.min_chunk_step_size, csi->chunk_step->integer_step.max_chunk_step_size, csi->number, TRUE, csi->chunk_step->integer_step.check_max, NULL, csi->position);
  }else{
    if ( csi->status == DUMPING_CHUNK ){
      new_minmax = csi->chunk_step->integer_step.type.sign.cursor + (csi->chunk_step->integer_step.type.sign.max - csi->chunk_step->integer_step.type.sign.cursor)/2 ;
      if ( new_minmax == csi->chunk_step->integer_step.type.sign.cursor )
        new_minmax++;
    }else{
      new_minmax = csi->chunk_step->integer_step.type.sign.min + (csi->chunk_step->integer_step.type.sign.max - csi->chunk_step->integer_step.type.sign.min)/2 ;
      if ( new_minmax == csi->chunk_step->integer_step.type.sign.min    )
        new_minmax++;
    }
    type.sign.min = new_minmax;

    new_csi = new_integer_step_item(FALSE, NULL, dbt->primary_key->data, csi->chunk_step->integer_step.is_unsigned, type, csi->deep + 1, csi->chunk_step->integer_step.is_step_fixed_length, csi->chunk_step->integer_step.step, csi->chunk_step->integer_step.min_chunk_step_size, csi->chunk_step->integer_step.max_chunk_step_size, csi->number+pow(2,csi->deep), TRUE, csi->chunk_step->integer_step.check_max, NULL, csi->position);

    csi->chunk_step->integer_step.check_max=TRUE;
  }

  csi->chunk_step->integer_step.type.sign.max = new_minmax - 1;

  common_to_chunk_step(dbt, csi, new_csi);

  return new_csi;
}


// dbt->chunks_mutex is LOCKED
struct chunk_step_item *get_next_integer_chunk(struct db_table *dbt){
  struct chunk_step_item *csi=NULL, *new_csi=NULL;
  if (dbt->chunks!=NULL){
    csi = (struct chunk_step_item *)g_async_queue_try_pop(dbt->chunks_queue);      
    while (csi!=NULL){
      g_mutex_lock(csi->mutex);
      if (csi->status==UNASSIGNED){
        csi->status=ASSIGNED;
        g_async_queue_push(dbt->chunks_queue, csi);
        g_mutex_unlock(csi->mutex);
        return csi;
      }
      if (csi->chunk_step->integer_step.is_unsigned) {

        if (csi->chunk_step->integer_step.type.unsign.cursor < csi->chunk_step->integer_step.type.unsign.max // it is not the last chunk
        && (  
             ( csi->status == DUMPING_CHUNK && (csi->chunk_step->integer_step.type.unsign.max - csi->chunk_step->integer_step.type.unsign.cursor ) > csi->chunk_step->integer_step.step // As this chunk is dumping data, another thread can continue with the remaining rows
             ) || 
             ( csi->status == ASSIGNED      && (csi->chunk_step->integer_step.type.unsign.max - csi->chunk_step->integer_step.type.unsign.min    ) > csi->chunk_step->integer_step.step // As this chunk is going to process another step, another thread can continue with the remaining rows
             )
           )
         ){
          new_csi=split_unsigned_chunk_step(dbt,csi);
          g_mutex_unlock(csi->mutex);
          return new_csi;
        }else{
//        g_message("Not able to split min %"G_GUINT64_FORMAT" step: %"G_GUINT64_FORMAT" max: %"G_GUINT64_FORMAT, cs->integer_step.nmin, cs->integer_step.step, cs->integer_step.nmax);
          if (dbt->multicolumn && csi->next && csi->next->chunk_type!=NONE){
            g_message("Not able to split previous level but can I split next level?\nStatus: %d / %d\nMax/Cursor/min: %"G_GUINT64_FORMAT"/%"G_GUINT64_FORMAT"/%"G_GUINT64_FORMAT" | Step: %"G_GUINT64_FORMAT,csi->status,DUMPING_CHUNK,csi->chunk_step->integer_step.type.unsign.max,csi->chunk_step->integer_step.type.unsign.cursor,csi->chunk_step->integer_step.type.unsign.min,csi->chunk_step->integer_step.step);
            
          }


          g_mutex_unlock(csi->mutex);
          if (csi->status==COMPLETED){
            free_integer_step_item(csi);
          }
        }
      }else{
        if (csi->chunk_step->integer_step.type.sign.cursor < csi->chunk_step->integer_step.type.sign.max // it is not the last chunk
        && (  
             ( csi->status == DUMPING_CHUNK && gint64_abs(csi->chunk_step->integer_step.type.sign.max - csi->chunk_step->integer_step.type.sign.cursor) > csi->chunk_step->integer_step.step // As this chunk is dumping data, another thread can continue with the remaining rows
             ) || 
             ( csi->status == ASSIGNED      && gint64_abs(csi->chunk_step->integer_step.type.sign.max - csi->chunk_step->integer_step.type.sign.min)    > csi->chunk_step->integer_step.step // As this chunk is going to process another step, another thread can continue with the remaining rows
             )
           )
         ){
          new_csi=split_signed_chunk_step(dbt,csi);
          g_mutex_unlock(csi->mutex);
          return new_csi;
        }else{
//        g_message("Not able to split min %"G_GUINT64_FORMAT" step: %"G_GUINT64_FORMAT" max: %"G_GUINT64_FORMAT, cs->integer_step.nmin, cs->integer_step.step, cs->integer_step.nmax);

          if (dbt->multicolumn){
            g_message("Not able to split previous level but can I split next level?");
          }

          g_mutex_unlock(csi->mutex);
          if (csi->status==COMPLETED){
            free_integer_step_item(csi);
          }
        }
      }
      csi = (struct chunk_step_item *)g_async_queue_try_pop(dbt->chunks_queue);
    }
  }
  return NULL;
}

void refresh_integer_min_max(MYSQL *conn, struct db_table *dbt, struct chunk_step_item *csi ){
  struct integer_step * ics = &(csi->chunk_step->integer_step);
  gchar *query = NULL;
  MYSQL_ROW row = NULL;
  MYSQL_RES *minmax = NULL;
  /* Get minimum/maximum */

//  GString *where = g_string_new("");
//  update_integer_where_on_gstring(where, csi->field, csi->chunk_step->integer_step.is_unsigned, csi->chunk_step->integer_step.type, );

  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s MIN(`%s`),MAX(`%s`) FROM `%s`.`%s` WHERE %s",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        csi->field, csi->field, dbt->database->name, dbt->table, csi->prefix->str));
//  g_message("refresh_integer_min_max:: %s", query);
  g_free(query);
  minmax = mysql_store_result(conn);

  if (!minmax){
    return;
  }
  row = mysql_fetch_row(minmax);

  if (row==NULL || row[0]==NULL){
    mysql_free_result(minmax);
    return;
  }
  if (ics->is_unsigned) {
    guint64 nmin = strtoull(row[0], NULL, 10);
    guint64 nmax = strtoull(row[1], NULL, 10);
    ics->type.unsign.min = nmin;
    ics->type.unsign.max = nmax;

  }else{
    gint64 nmin = strtoll(row[0], NULL, 10);
    gint64 nmax = strtoll(row[1], NULL, 10);
    ics->type.sign.min = nmin;
    ics->type.sign.max = nmax;
  }
  mysql_free_result(minmax);
}


void update_integer_min(MYSQL *conn, struct db_table *dbt, struct chunk_step_item *csi ){
//  union chunk_step *cs= tj->chunk_step;
  struct integer_step * ics = &(csi->chunk_step->integer_step);
  gchar *query = NULL;
  MYSQL_ROW row = NULL;
  MYSQL_RES *minmax = NULL;
  /* Get minimum/maximum */

  GString *where = g_string_new("");
  update_integer_where_on_gstring(where, csi->prefix, csi->field, csi->chunk_step->integer_step.is_unsigned, csi->chunk_step->integer_step.type, FALSE);

  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE %s ORDER BY `%s` ASC LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        csi->field, dbt->database->name, dbt->table, where->str, csi->field));
  g_free(query);
  minmax = mysql_store_result(conn);

  if (!minmax){
    return;
  }
  row = mysql_fetch_row(minmax);

  if (row==NULL || row[0]==NULL){
    mysql_free_result(minmax);
    return;
  }
  if (ics->is_unsigned) {
    guint64 nmin = strtoull(row[0], NULL, 10);
    ics->type.unsign.min = nmin;
  }else{
    gint64 nmin = strtoll(row[0], NULL, 10);
    ics->type.sign.min = nmin;
  }
  mysql_free_result(minmax);
}

void update_integer_max(MYSQL *conn,struct db_table *dbt, struct chunk_step_item *csi ){
  struct integer_step * ics = &(csi->chunk_step->integer_step);
  gchar *query = NULL;
  MYSQL_ROW row = NULL;
  MYSQL_RES *minmax = NULL;
  /* Get minimum/maximum */

  GString *where = g_string_new("");
  update_integer_where_on_gstring(where, csi->prefix, csi->field, csi->chunk_step->integer_step.is_unsigned, csi->chunk_step->integer_step.type, FALSE);

  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE %s ORDER BY `%s` DESC LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        csi->field, dbt->database->name, dbt->table, where->str, csi->field));
  minmax = mysql_store_result(conn);
  g_free(query);

  if (!minmax){
//    g_message("No middle point");
    goto cleanup;
  }
  row = mysql_fetch_row(minmax);

  if (row==NULL || row[0]==NULL){
//    g_message("No middle point");
cleanup:

    if (ics->is_unsigned) {
      ics->type.unsign.max = ics->type.unsign.min;
    }else{
      ics->type.sign.max = ics->type.sign.min;
    }

    mysql_free_result(minmax);
    return;
  }

  if (ics->is_unsigned) {
    guint64 nmax = strtoull(row[0], NULL, 10);
    ics->type.unsign.max = nmax;
  }else{
    gint64 nmax = strtoll(row[0], NULL, 10);
    ics->type.sign.max = nmax;
  }

  mysql_free_result(minmax);
}


void update_where_on_integer_step(struct chunk_step_item * csi);

guint process_integer_chunk_step(struct table_job *tj, struct chunk_step_item *csi){
  struct thread_data *td = tj->td;
  union chunk_step *cs = csi->chunk_step;

  check_pause_resume(td);
  if (shutdown_triggered) {
    return 1;
  }

// Stage 1: Update min and max if needed

  g_mutex_lock(csi->mutex);
//  if (tj->status == COMPLETED)
//    m_critical("Thread %d: Trying to process COMPLETED chunk",td->thread_id);
  csi->status = DUMPING_CHUNK;

  if (cs->integer_step.check_max){
//    g_message("thread: %d Updating MAX", td->thread_id);
    update_integer_max(td->thrconn, tj->dbt, csi);
    cs->integer_step.check_max=FALSE;
  }
  if (cs->integer_step.check_min){
//    g_message("thread: %d Updating MIN", td->thread_id);
    update_integer_min(td->thrconn, tj->dbt, csi);
//    g_message("thread: %d New MIN: %ld", td->thread_id, tj->chunk_step->integer_step.nmin);
    cs->integer_step.check_min=FALSE;
  }

// Stage 2: Setting cursor

if (cs->integer_step.is_unsigned){

//  tj->chunk_step->integer_step.type.unsign.cursor = (tj->chunk_step->integer_step.type.unsign.min + tj->chunk_step->integer_step.step) > tj->chunk_step->integer_step.type.unsign.max ? tj->chunk_step->integer_step.type.unsign.max : tj->chunk_step->integer_step.type.unsign.min + tj->chunk_step->integer_step.step;
  if (cs->integer_step.step -1 > cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.min)
    cs->integer_step.type.unsign.cursor = cs->integer_step.type.unsign.max;
  else
    cs->integer_step.type.unsign.cursor = cs->integer_step.type.unsign.min + cs->integer_step.step -1;
  cs->integer_step.estimated_remaining_steps=(cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.cursor) / cs->integer_step.step;

}else{

//  tj->chunk_step->integer_step.type.sign.cursor = ((gint64)(tj->chunk_step->integer_step.type.sign.min + tj->chunk_step->integer_step.step)) > tj->chunk_step->integer_step.type.sign.max ? tj->chunk_step->integer_step.type.sign.max : tj->chunk_step->integer_step.type.sign.min + (gint64) tj->chunk_step->integer_step.step;
  if (cs->integer_step.step - 1 > gint64_abs(cs->integer_step.type.sign.max - cs->integer_step.type.sign.min))
    cs->integer_step.type.sign.cursor = cs->integer_step.type.sign.max;
  else
    cs->integer_step.type.sign.cursor = cs->integer_step.type.sign.min + cs->integer_step.step - 1;
//g_message("cs->integer_step.type.sign.min: %"G_GINT64_FORMAT" | cs->integer_step.type.sign.cursor: %"G_GINT64_FORMAT "| cs->integer_step.type.sign.max: %"G_GINT64_FORMAT, cs->integer_step.type.sign.min,cs->integer_step.type.sign.cursor, cs->integer_step.type.sign.max);

  cs->integer_step.estimated_remaining_steps=(cs->integer_step.type.sign.max - cs->integer_step.type.sign.cursor) / cs->integer_step.step;
}

  g_mutex_unlock(csi->mutex);
/*  if (tj->chunk_step->integer_step.nmin == tj->chunk_step->integer_step.nmax){
    return;
  }*/
//  g_message("CONTINUE");


  update_estimated_remaining_chunks_on_dbt(tj->dbt);

//  g_string_append(tj->where,get_integer_chunk_where(cs));

// Step 3: Executing query and writing data
  update_where_on_integer_step(csi);
  
//  guint64 rows=get_rows_from_explain(td->thrconn, tj->dbt, csi->where, csi->field);
//  g_message("Rows in chunk on %s.%s : %"G_GUINT64_FORMAT, tj->dbt->database->name, tj->dbt->table, rows);

//  (void)rows;

  if (csi->next !=NULL){
    refresh_integer_min_max(td->thrconn, tj->dbt, csi->next);
    csi->next->chunk_functions.process( tj , csi->next);
  }else{
    g_string_set_size(tj->where,0);
    g_string_append(tj->where, csi->where->str);

    if (cs->integer_step.is_step_fixed_length) {
      write_table_job_into_file(tj);
    }else{
 
      GDateTime *from = g_date_time_new_now_local();
      write_table_job_into_file(tj);
      GDateTime *to = g_date_time_new_now_local();

// Step 3.1: Updating Step length

      GTimeSpan diff=g_date_time_difference(to,from)/G_TIME_SPAN_SECOND;
      g_date_time_unref(from);
      g_date_time_unref(to);
      if (diff > 2){
        cs->integer_step.step=cs->integer_step.step  / 2;
        cs->integer_step.step=cs->integer_step.step<csi->chunk_step->integer_step.min_chunk_step_size?csi->chunk_step->integer_step.min_chunk_step_size:cs->integer_step.step;
//    g_message("Decreasing time: %ld | %ld", diff, tj->chunk_step->integer_step.step);
      }else if (diff < 1){
        cs->integer_step.step=cs->integer_step.step  * 2 == 0?cs->integer_step.step:cs->integer_step.step  * 2;
        if (max_chunk_step_size!=0)
          cs->integer_step.step=cs->integer_step.step>csi->chunk_step->integer_step.max_chunk_step_size?csi->chunk_step->integer_step.max_chunk_step_size:cs->integer_step.step;
//    g_message("Increasing time: %ld | %ld", diff, tj->chunk_step->integer_step.step);
      }
    }
  }

// Step 5: Updating min

  g_mutex_lock(csi->mutex);
  if (csi->status != COMPLETED)
    csi->status = ASSIGNED;
  if (cs->integer_step.is_unsigned){
    cs->integer_step.type.unsign.min=cs->integer_step.type.unsign.cursor+1;
  }else{
    cs->integer_step.type.sign.min=cs->integer_step.type.sign.cursor+1;
  }
  g_mutex_unlock(csi->mutex);
  return 0;
}

void process_integer_chunk(struct table_job *tj, struct chunk_step_item *csi){
  struct thread_data *td = tj->td;
  struct db_table *dbt = tj->dbt;
  union chunk_step *cs = csi->chunk_step;
  gboolean multicolumn_process=FALSE;

  if (dbt->multicolumn && g_list_length(dbt->primary_key) - 1 > csi->position){
    update_integer_where_on_gstring(csi->where, csi->prefix, csi->field, csi->chunk_step->integer_step.is_unsigned, csi->chunk_step->integer_step.type, FALSE);
    guint64 rows=get_rows_from_explain(td->thrconn, tj->dbt, csi->where, csi->field);
    if (rows > csi->chunk_step->integer_step.min_chunk_step_size){
      struct chunk_step_item *next_csi = initialize_chunk_step_item(td->thrconn, dbt, csi->position + 1, csi->where, rows);
      if (next_csi && next_csi->chunk_type!=NONE){
        csi->next=next_csi;
        multicolumn_process=TRUE;
      }
    }
  }


  // First step, we need this to process the one time prefix
  g_string_set_size(tj->where,0);
  if (process_integer_chunk_step(tj, csi)){
    g_message("Thread %d: Job has been cacelled",td->thread_id);
    return;
  }
  g_atomic_int_inc(dbt->chunks_completed);
//  if (csi->prefix)
//    g_free(csi->prefix);
//  csi->prefix=NULL;
  csi->include_null=FALSE;

  // Processing the remaining steps
  g_mutex_lock(csi->mutex);
  if (cs->integer_step.is_unsigned){
    // Remaining unsigned steps
//g_message("cs->integer_step.type.unsign.min: %"G_GUINT64_FORMAT" | cs->integer_step.type.unsign.max: %"G_GUINT64_FORMAT, cs->integer_step.type.unsign.min, cs->integer_step.type.unsign.max);
    while ( 
              (  cs->integer_step.is_unsigned && cs->integer_step.type.unsign.min <= cs->integer_step.type.unsign.max )
           || ( !cs->integer_step.is_unsigned && cs->integer_step.type.sign.min   <= cs->integer_step.type.sign.max   )
          ){
      g_mutex_unlock(csi->mutex);
      g_string_set_size(tj->where,0);
      if (process_integer_chunk_step(tj, csi)){
        g_message("Thread %d: Job has been cacelled",td->thread_id);
        return;
      }
      g_atomic_int_inc(dbt->chunks_completed);
      g_mutex_lock(csi->mutex);
    }
  }
  g_mutex_unlock(csi->mutex);



/*
  g_mutex_lock(tj->mutex);
  if (cs->integer_step.step == 1){
    g_mutex_unlock(tj->mutex);
    g_string_set_size(tj->where,0);
    if (process_integer_chunk_step(tj)){
      g_message("Thread %d: Job has been cacelled",td->thread_id);
      return;
    }
    g_atomic_int_inc(dbt->chunks_completed);
  }else{
    g_mutex_unlock(tj->mutex);
  }
*/
  if (multicolumn_process){
    free_integer_step_item(csi->next); 
    csi->next=NULL;
  }

  if (csi->position==0){
    g_mutex_lock(dbt->chunks_mutex);
    g_mutex_lock(csi->mutex);
//  dbt->chunks=g_list_remove(dbt->chunks,cs);
    cs->integer_step.estimated_remaining_steps=0;
    if (g_list_length(dbt->chunks) == 0){
      g_message("Thread %d: Table %s completed ",td->thread_id,dbt->table);
      dbt->chunks=NULL;
    }
    // Chunk completed
    csi->status=COMPLETED;
    g_mutex_unlock(dbt->chunks_mutex);
    g_mutex_unlock(csi->mutex);
  }
}

void update_integer_where_on_gstring(GString *where, GString *prefix, gchar * field, gboolean is_unsigned, union type type, gboolean use_cursor){
  union type t;  
  if (prefix && prefix->len>0)
    g_string_append_printf(where,"(%s AND ",
                          prefix->str);

  g_string_append(where,"(");
  if (is_unsigned){
      t.unsign.min = type.unsign.min;
      if (!use_cursor)
        t.unsign.cursor = type.unsign.max;
      else
        t.unsign.cursor = type.unsign.cursor;
      if (t.unsign.min == t.unsign.cursor) {
                g_string_append_printf(where, "`%s` = %"G_GUINT64_FORMAT,
                          field, t.unsign.cursor);
      }else{
                g_string_append_printf(where,"%"G_GUINT64_FORMAT" <= `%s` AND `%s` <= %"G_GUINT64_FORMAT,
                          t.unsign.min, field,
                          field, t.unsign.cursor);
      }
  }else{
      t.sign.min = type.sign.min;
      if (!use_cursor)
        t.sign.cursor = type.sign.max;
      else
        t.sign.cursor = type.sign.cursor;
      if (t.sign.min == t.sign.cursor){
                g_string_append_printf(where,"`%s` = %"G_GINT64_FORMAT,
                          field, t.sign.cursor);
      }else{
                g_string_append_printf(where,"%"G_GINT64_FORMAT" <= `%s` AND `%s` <= %"G_GINT64_FORMAT,
                          t.sign.min, field,
                          field, t.sign.cursor);
      }
  }
  g_string_append(where,")");
  if (prefix && prefix->len>0)
    g_string_append(where,")");
}

void update_where_on_integer_step(struct chunk_step_item * csi){
  struct integer_step *chunk_step=&(csi->chunk_step->integer_step);
  g_string_set_size(csi->where,0);
  if (csi->include_null)
    g_string_append_printf(csi->where,"(`%s` IS NULL OR",csi->field);
  update_integer_where_on_gstring(csi->where, csi->prefix, csi->field, chunk_step->is_unsigned, chunk_step->type, TRUE);
  if (csi->include_null)
    g_string_append(csi->where,")");
}

/*
gchar * get_integer_chunk_where(union chunk_step * chunk_step){
  update_where_on_integer_step(&(chunk_step->integer_step));
  return g_strdup_printf("(%s(%s%s%s))",
                          chunk_step->integer_step.include_null?chunk_step->integer_step.include_null:"",
                          chunk_step->integer_step.prefix?chunk_step->integer_step.prefix:"", chunk_step->integer_step.prefix?" AND ":"",
                          chunk_step->integer_step.where->str);
}
*/
