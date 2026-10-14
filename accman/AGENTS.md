# accman

## Purpose
- Simple web UI for Astonia account and character management.
- Runs with PHP built-in web server in userland.

## Start/Stop
- Start: `./start.sh`
- Stop: `./stop.sh`
- Default bind: `0.0.0.0:8088`

## Access Control
- Login required for all pages except account creation.
- Login accepts account ID or email plus password.
- Account ID 1 is admin.
  - Can view the accounts list.
  - Can view any account page.
  - Can create god characters.
- Non-admin users:
  - May only view their own account page.
  - Cannot create god characters.
- Logout is POST-only with CSRF protection.

## Character Rules
- Max 20 characters per account (creation disabled at 20).
 - Character creation requires a CSRF token and only allows posting to the user's own account unless admin.

## Files
- App entry: `public/index.php`
- Config: `config.php`
- Install notes: `install.md`
- Service scripts: `start.sh`, `stop.sh`
