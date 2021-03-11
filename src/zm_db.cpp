//
// ZoneMinder MySQL Implementation, $Date$, $Revision$
// Copyright (C) 2001-2008 Philip Coombes
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// 
#include "zm_db.h"

#include "zm_logger.h"
#include "zm_signal.h"
#include <cstdlib>

MYSQL dbconn;
std::mutex db_mutex;
zmDbQueue  dbQueue;

bool zmDbConnected = false;

bool zmDbConnect() {
  // For some reason having these lines causes memory corruption and crashing on newer debian/ubuntu
	// But they really need to be here in order to prevent a double open of mysql
  if ( zmDbConnected )  {
    //Warning("Calling zmDbConnect when already connected");
    return true;
  }

  if ( !mysql_init(&dbconn) ) {
    Error("Can't initialise database connection: %s", mysql_error(&dbconn));
    return false;
  }

  bool reconnect = 1;
  if ( mysql_options(&dbconn, MYSQL_OPT_RECONNECT, &reconnect) )
    Error("Can't set database auto reconnect option: %s", mysql_error(&dbconn));

  if ( !staticConfig.DB_SSL_CA_CERT.empty() ) {
    mysql_ssl_set(&dbconn,
        staticConfig.DB_SSL_CLIENT_KEY.c_str(),
        staticConfig.DB_SSL_CLIENT_CERT.c_str(),
        staticConfig.DB_SSL_CA_CERT.c_str(),
        nullptr, nullptr);
  }

  std::string::size_type colonIndex = staticConfig.DB_HOST.find(":");
  if ( colonIndex == std::string::npos ) {
    if ( !mysql_real_connect(
          &dbconn,
          staticConfig.DB_HOST.c_str(),
          staticConfig.DB_USER.c_str(),
          staticConfig.DB_PASS.c_str(),
          nullptr, 0, nullptr, 0) ) {
      Error("Can't connect to server: %s", mysql_error(&dbconn));
      mysql_close(&dbconn);
      return false;
    }
  } else {
    std::string dbHost = staticConfig.DB_HOST.substr(0, colonIndex);
    std::string dbPortOrSocket = staticConfig.DB_HOST.substr(colonIndex+1);
    if ( dbPortOrSocket[0] == '/' ) {
      if ( !mysql_real_connect(
            &dbconn,
            nullptr,
            staticConfig.DB_USER.c_str(),
            staticConfig.DB_PASS.c_str(),
            nullptr, 0, dbPortOrSocket.c_str(), 0) ) {
        Error("Can't connect to server: %s", mysql_error(&dbconn));
        mysql_close(&dbconn);
        return false;
      }
    } else {
      if ( !mysql_real_connect(
            &dbconn,
            dbHost.c_str(),
            staticConfig.DB_USER.c_str(),
            staticConfig.DB_PASS.c_str(),
            nullptr,
            atoi(dbPortOrSocket.c_str()),
            nullptr, 0) ) {
        Error("Can't connect to server: %s", mysql_error(&dbconn));
        mysql_close(&dbconn);
        return false;
      }
    }
  }
  if ( mysql_select_db(&dbconn, staticConfig.DB_NAME.c_str()) ) {
    Error("Can't select database: %s", mysql_error(&dbconn));
    mysql_close(&dbconn);
    return false;
  }
  if ( mysql_query(&dbconn, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED") ) {
    Error("Can't set isolation level: %s", mysql_error(&dbconn));
  }
  zmDbConnected = true;
  return zmDbConnected;
}

void zmDbClose() {
  if (zmDbConnected) {
    std::lock_guard<std::mutex> lck(db_mutex);
    mysql_close(&dbconn);
    // mysql_init() call implicitly mysql_library_init() but
    // mysql_close() does not call mysql_library_end()
    mysql_library_end();
    zmDbConnected = false;
  }
}

MYSQL_RES * zmDbFetch(const char * query) {
  std::lock_guard<std::mutex> lck(db_mutex);
  if (!zmDbConnected) {
    Error("Not connected.");
    return nullptr;
  }

  if (mysql_query(&dbconn, query)) {
    Error("Can't run query: %s", mysql_error(&dbconn));
    return nullptr;
  }
  MYSQL_RES *result = mysql_store_result(&dbconn);
  if (!result) {
    Error("Can't use query result: %s for query %s", mysql_error(&dbconn), query);
  }
  return result;
} // end MYSQL_RES * zmDbFetch(const char * query);

zmDbRow *zmDbFetchOne(const char *query) {
  zmDbRow *row = new zmDbRow();
  if (row->fetch(query)) {
    return row;
  } 
  delete row;
  return nullptr;
}

MYSQL_RES *zmDbRow::fetch(const char *query) {
  result_set = zmDbFetch(query);
  if (!result_set) return result_set;

  int n_rows = mysql_num_rows(result_set);
  if (n_rows != 1) {
    Error("Bogus number of lines return from query, %d returned for query %s.", n_rows, query);
    mysql_free_result(result_set);
    result_set = nullptr;
    return result_set;
  }

  row = mysql_fetch_row(result_set);
  if (!row) {
    mysql_free_result(result_set);
    result_set = nullptr;
    Error("Error getting row from query %s. Error is %s", query, mysql_error(&dbconn));
  } else {
    Debug(5, "Success");
  }
  return result_set;
}

int zmDbDo(const char *query) {
  std::lock_guard<std::mutex> lck(db_mutex);
  if (!zmDbConnected)
    return 0;
  int rc;
  while ((rc = mysql_query(&dbconn, query)) and !zm_terminate) {
    Logger *logger = Logger::fetch();
    Logger::Level oldLevel = logger->databaseLevel();
    logger->databaseLevel(Logger::NOLOG);
    Error("Can't run query %s: %s", query, mysql_error(&dbconn));
    logger->databaseLevel(oldLevel);
    if ( (mysql_errno(&dbconn) != ER_LOCK_WAIT_TIMEOUT) ) {
      return rc;
    }
  }
  Logger *logger = Logger::fetch();
  Logger::Level oldLevel = logger->databaseLevel();
  logger->databaseLevel(Logger::NOLOG);

  Debug(1, "Success running sql query %s", query);
  logger->databaseLevel(oldLevel);
  return 1;
}

int zmDbDoInsert(const char *query) {
  std::lock_guard<std::mutex> lck(db_mutex);
  if (!zmDbConnected) return 0;
  int rc;
  while ( (rc = mysql_query(&dbconn, query)) and !zm_terminate) {
    Error("Can't run query %s: %s", query, mysql_error(&dbconn));
    if ( (mysql_errno(&dbconn) != ER_LOCK_WAIT_TIMEOUT) )
      return 0;
  }
  int id = mysql_insert_id(&dbconn);
  Debug(2, "Success running sql insert %s. Resulting id is %d", query, id);
  return id;
}

int zmDbDoUpdate(const char *query) {
  std::lock_guard<std::mutex> lck(db_mutex);
  if (!zmDbConnected) return 0;
  int rc;
  while ( (rc = mysql_query(&dbconn, query)) and !zm_terminate) {
    Error("Can't run query %s: %s", query, mysql_error(&dbconn));
    if ( (mysql_errno(&dbconn) != ER_LOCK_WAIT_TIMEOUT) )
      return -rc;
  }
  int affected = mysql_affected_rows(&dbconn);
  Debug(2, "Success running sql update %s. Rows modified %d", query, affected);
  return affected;
}

zmDbRow::~zmDbRow() {
  if (result_set) {
    mysql_free_result(result_set);
    result_set = nullptr;
  }
  row = nullptr;
}

zmDbQueue::zmDbQueue() :
  mThread(&zmDbQueue::process, this),
  mTerminate(false)
{ }

zmDbQueue::~zmDbQueue() {
  mTerminate = true;
  mCondition.notify_all();
  mThread.join();
}

void zmDbQueue::process() {
  std::unique_lock<std::mutex> lock(mMutex);

  while (!mTerminate and !zm_terminate) {
    if (mQueue.empty()) {
      mCondition.wait(lock);
    }
    if (!mQueue.empty()) {
      std::string sql = mQueue.front();
      mQueue.pop();
      lock.unlock();
      zmDbDo(sql.c_str());
      lock.lock();
    }
  }
}  // end void zmDbQueue::process()

void zmDbQueue::push(std::string &&sql) {
  std::unique_lock<std::mutex> lock(mMutex);
  mQueue.push(std::move(sql));
  mCondition.notify_all();
}
