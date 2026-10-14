/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 *
 * Connects to the database with the settings from config_data.
 * Include config.h before this file.
 */

#ifndef DBCONNECT_H
#define DBCONNECT_H

#include <mysql/mysql.h>

// MariaDB Connector/C 3.4 and newer insist on a verified TLS connection by
// default and refuse a MariaDB server without a certificate ("SSL is required,
// but the server does not support it"). The database runs on the same machine
// or a private network, so we turn that off. Must be called after mysql_init().
static inline MYSQL *db_connect(MYSQL *my) {
#ifdef MARIADB_PACKAGE_VERSION
    my_bool off = 0;
    mysql_optionsv(my, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &off);
#endif
    return mysql_real_connect(my, config_data.dbhost, config_data.dbuser, config_data.dbpass, config_data.dbname, 0, NULL, 0);
}

#endif
