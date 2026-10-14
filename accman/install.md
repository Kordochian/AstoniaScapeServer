# Account Manager (accman)

## Requirements (Ubuntu 22.04)
- PHP 8.1+ with MySQL driver and Argon2id support
  - Packages: `php8.1-cli`, `php8.1-mysql`
- MySQL or MariaDB with the Astonia schema loaded (`create_tables.sql` / `create_tables.sql`)

## Setup
1. Edit `accman/config.php` with your database credentials.
2. Start the service: `./start.sh`
3. Open `http://<server-ip>:8088` in your browser.

## Stop
- Run `./stop.sh`
