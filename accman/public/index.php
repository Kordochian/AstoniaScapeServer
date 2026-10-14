<?php

declare(strict_types=1);

$config = require __DIR__ . '/../config.php';

ini_set('session.cookie_httponly', '1');
ini_set('session.cookie_samesite', 'Lax');
session_set_cookie_params([
    'lifetime' => 0,
    'path' => '/',
    'secure' => false,
    'httponly' => true,
    'samesite' => 'Lax',
]);
session_start();

const CF_GOD = 1 << 2;
const CF_MALE = 1 << 11;
const CF_FEMALE = 1 << 12;
const CF_WARRIOR = 1 << 13;
const CF_MAGE = 1 << 14;

const DRD_JUNK_PPD = 0x81000072;

function db(array $config): PDO {
    static $pdo = null;
    if ($pdo instanceof PDO) {
        return $pdo;
    }
    $dsn = sprintf(
        'mysql:host=%s;dbname=%s;charset=utf8mb4',
        $config['db_host'],
        $config['db_name']
    );
    $pdo = new PDO($dsn, $config['db_user'], $config['db_pass'], [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    ]);
    return $pdo;
}

function h($value): string {
    return htmlspecialchars((string)$value, ENT_QUOTES, 'UTF-8');
}

function redirect(string $url): void {
    header('Location: ' . $url);
    exit;
}

function csrf_token(): string {
    if (empty($_SESSION['csrf_token'])) {
        $_SESSION['csrf_token'] = bin2hex(random_bytes(32));
    }
    return $_SESSION['csrf_token'];
}

function require_csrf(string $page): void {
    $posted = (string)($_POST['csrf_token'] ?? '');
    $session = (string)($_SESSION['csrf_token'] ?? '');
    if ($posted === '' || $session === '' || !hash_equals($session, $posted)) {
        redirect('?page=' . rawurlencode($page) . '&err=' . rawurlencode('Invalid session token.'));
    }
}

function build_flag(string $gender, string $class, bool $is_god): int {
    $flag = 0;
    if ($is_god) {
        $flag |= CF_GOD;
    }
    if ($class === 'W') {
        $flag |= CF_WARRIOR;
    } else {
        $flag |= CF_MAGE;
    }
    if ($gender === 'M') {
        $flag |= CF_MALE;
    } else {
        $flag |= CF_FEMALE;
    }
    return $flag;
}

function flag_blob(int $flag): string {
    $lo = $flag & 0xffffffff;
    $hi = ($flag >> 32) & 0xffffffff;
    return pack('V', $lo) . pack('V', $hi);
}

function ppd_blob(): string {
    return pack('V', DRD_JUNK_PPD) . pack('V', 16) . str_repeat("\0", 16);
}

function class_label(int $flag): string {
    $gender = ($flag & CF_MALE) ? 'Male' : 'Female';
    $class = ($flag & CF_WARRIOR) ? 'Warrior' : 'Mage';
    $god = ($flag & CF_GOD) ? 'God' : 'Mortal';
    return $gender . ' ' . $class . ' (' . $god . ')';
}

function fmt_time(int $timestamp): string {
    if ($timestamp <= 0) {
        return '-';
    }
    return date('Y-m-d H:i', $timestamp);
}

$page = $_GET['page'] ?? 'home';
$message = $_GET['msg'] ?? '';
$error = $_GET['err'] ?? '';
if (isset($_SESSION['accman_user']) && $page === 'home') {
    $page = 'account';
}
if (!isset($_SESSION['accman_user']) && !in_array($page, ['login', 'account_create'], true)) {
    $page = 'login';
}

try {
    $pdo = db($config);
} catch (Throwable $e) {
    $error = 'Database connection failed. Check config.php and MySQL status.';
    $pdo = null;
}

if ($_SERVER['REQUEST_METHOD'] === 'POST' && $pdo) {
    require_csrf($page);

    if ($page === 'login') {
        $identifier = trim((string)($_POST['identifier'] ?? ''));
        $password = (string)($_POST['password'] ?? '');

        if ($identifier === '' || $password === '') {
            redirect('?page=login&err=' . rawurlencode('ID or email and password are required.'));
        }

        if (ctype_digit($identifier)) {
            $stmt = $pdo->prepare('SELECT ID, email, password FROM account WHERE ID = ?');
            $stmt->execute([(int)$identifier]);
        } else {
            $stmt = $pdo->prepare('SELECT ID, email, password FROM account WHERE email = ?');
            $stmt->execute([strtolower($identifier)]);
        }
        $row = $stmt->fetch();
        if (!$row || !password_verify($password, $row['password'])) {
            redirect('?page=login&err=' . rawurlencode('Invalid credentials.'));
        }

        session_regenerate_id(true);
        $_SESSION['accman_user'] = [
            'id' => (int)$row['ID'],
            'email' => (string)$row['email'],
        ];
        redirect('?page=account&id=' . (int)$row['ID'] . '&msg=' . rawurlencode('Logged in.'));
    }

    if ($page === 'account_create') {
        $email = trim((string)($_POST['email'] ?? ''));
        $password = (string)($_POST['password'] ?? '');
        $email = strtolower($email);

        if ($email === '' || $password === '') {
            redirect('?page=account_create&err=' . rawurlencode('Email and password are required.'));
        }
        if (!filter_var($email, FILTER_VALIDATE_EMAIL)) {
            redirect('?page=account_create&err=' . rawurlencode('Invalid email format.'));
        }
        if (strlen($password) < 7) {
            redirect('?page=account_create&err=' . rawurlencode('Password must be at least 7 characters.'));
        }

        $stmt = $pdo->prepare('SELECT ID FROM account WHERE email = ?');
        $stmt->execute([$email]);
        if ($stmt->fetch()) {
            redirect('?page=account_create&err=' . rawurlencode('Email is already in use.'));
        }

        $hash = password_hash($password, PASSWORD_ARGON2ID);
        if ($hash === false) {
            redirect('?page=account_create&err=' . rawurlencode('Password hashing failed.'));
        }

        $stmt = $pdo->prepare('INSERT INTO account (email, password, creation_time) VALUES (?, ?, ?)');
        $stmt->execute([$email, $hash, time()]);
        $new_id = (int)$pdo->lastInsertId();

        redirect('?page=account&id=' . $new_id . '&msg=' . rawurlencode('Account created.'));
    }

    if ($page === 'character_create') {
        $account_id = (int)($_POST['account_id'] ?? 0);
        $name = trim((string)($_POST['name'] ?? ''));
        $gender = strtoupper((string)($_POST['gender'] ?? ''));
        $class = strtoupper((string)($_POST['class'] ?? ''));
        $is_god = ($_POST['is_god'] ?? '') === '1';
        $session_id = (int)($_SESSION['accman_user']['id'] ?? 0);

        if ($session_id !== 1 && $account_id !== $session_id) {
            redirect('?page=account&id=' . $session_id . '&err=' . rawurlencode('Access denied.'));
        }
        if ($account_id <= 0 || $name === '') {
            redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Name is required.'));
        }
        if (strlen($name) < 2 || strlen($name) > 40) {
            redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Name must be 2-40 characters.'));
        }
        if (!in_array($gender, ['M', 'F'], true) || !in_array($class, ['W', 'M'], true)) {
            redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Invalid class or gender.'));
        }
        if ($is_god && $session_id !== 1) {
            redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Only admin may create god characters.'));
        }

        $stmt = $pdo->prepare('SELECT COUNT(*) AS c FROM chars WHERE sID = ?');
        $stmt->execute([$account_id]);
        $char_count = (int)$stmt->fetch()['c'];
        if ($char_count >= 20) {
            redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Character limit reached.'));
        }

        $flag = build_flag($gender, $class, $is_god);
        $flag32 = $flag & 0xffffffff;
        $flag_blob = flag_blob($flag);
        $ppd = ppd_blob();
        $mirror = random_int(1, 26);

        $stmt = $pdo->prepare(
            'INSERT INTO chars (' .
            'name, class, sID, karma, clan, clan_rank, clan_serial, experience,' .
            'current_area, allowed_area, creation_time, login_time, logout_time,' .
            'locked, chr, item, ppd, mirror, current_mirror' .
            ') VALUES (' .
            '?, ?, ?, 0, 0, 0, 0, 0,' .
            '0, 1, ?, 1, 1,' .
            '\'N\', ?, ?, ?, ?, 0)'
        );

        try {
            $stmt->execute([
                $name,
                $flag32,
                $account_id,
                time(),
                $flag_blob,
                $flag_blob,
                $ppd,
                $mirror,
            ]);
        } catch (PDOException $e) {
            $err_code = $e->errorInfo[1] ?? 0;
            if ((int)$err_code === 1062) {
                redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Character name already exists.'));
            }
            redirect('?page=account&id=' . $account_id . '&err=' . rawurlencode('Failed to create character.'));
        }

        redirect('?page=account&id=' . $account_id . '&msg=' . rawurlencode('Character created.'));
    }
}

if ($page === 'logout' && $_SERVER['REQUEST_METHOD'] === 'POST') {
    require_csrf('login');
    $_SESSION = [];
    if (ini_get('session.use_cookies')) {
        $params = session_get_cookie_params();
        setcookie(session_name(), '', time() - 42000, $params['path'], $params['domain'], $params['secure'], $params['httponly']);
    }
    session_destroy();
    redirect('?page=login&msg=' . rawurlencode('Logged out.'));
}
if ($page === 'logout') {
    redirect('?');
}

$counts = ['accounts' => 0, 'chars' => 0];
if ($pdo) {
    $counts['accounts'] = (int)$pdo->query('SELECT COUNT(*) AS c FROM account')->fetch()['c'];
    $counts['chars'] = (int)$pdo->query('SELECT COUNT(*) AS c FROM chars')->fetch()['c'];
}

?><!doctype html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Astonia Account Manager</title>
    <style>
        :root { color-scheme: light; }
        body { font-family: Arial, sans-serif; margin: 24px; color: #222; }
        header { margin-bottom: 20px; }
        nav a { margin-right: 12px; }
        .panel { border: 1px solid #ccc; padding: 16px; border-radius: 6px; margin-bottom: 16px; }
        .notice { background: #eef7ee; border: 1px solid #b9d8b9; padding: 8px; margin-bottom: 12px; }
        .error { background: #fdecec; border: 1px solid #e4bcbc; padding: 8px; margin-bottom: 12px; }
        table { border-collapse: collapse; width: 100%; }
        th, td { padding: 8px; border-bottom: 1px solid #ddd; text-align: left; }
        form > div { margin-bottom: 10px; }
        input[type="text"], input[type="email"], input[type="password"], select { width: 100%; max-width: 360px; padding: 6px; }
        .small { color: #666; font-size: 0.9em; }
    </style>
</head>
<body>
<header>
    <h1>Astonia Account Manager</h1>
    <nav>
        <?php if (isset($_SESSION['accman_user'])): ?>
            <a href="?">Home</a>
            <?php if ((int)$_SESSION['accman_user']['id'] === 1): ?>
                <a href="?page=accounts">Accounts</a>
            <?php endif; ?>
            <form method="post" action="?page=logout" style="display:inline">
                <input type="hidden" name="csrf_token" value="<?php echo h(csrf_token()); ?>">
                <button type="submit">Logout</button>
            </form>
        <?php else: ?>
            <a href="?page=login">Login</a>
            <a href="?page=account_create">Create Account</a>
        <?php endif; ?>
    </nav>
</header>

<?php if ($message !== ''): ?>
    <div class="notice"><?php echo h($message); ?></div>
<?php endif; ?>
<?php if ($error !== ''): ?>
    <div class="error"><?php echo h($error); ?></div>
<?php endif; ?>

<?php if ($page === 'login'): ?>
    <div class="panel">
        <strong>Login</strong>
        <form method="post" action="?page=login">
            <input type="hidden" name="csrf_token" value="<?php echo h(csrf_token()); ?>">
            <div>
                <label>ID or Email</label><br>
                <input type="text" name="identifier" required>
            </div>
            <div>
                <label>Password</label><br>
                <input type="password" name="password" required>
            </div>
            <button type="submit">Login</button>
        </form>
    </div>
<?php elseif ($page === 'home'): ?>
    <div class="panel">
        <strong>Overview</strong>
        <div class="small">Accounts: <?php echo h($counts['accounts']); ?> | Characters: <?php echo h($counts['chars']); ?></div>
    </div>
<?php elseif ($page === 'accounts' && $pdo): ?>
    <?php if ((int)($_SESSION['accman_user']['id'] ?? 0) !== 1): ?>
        <div class="error">Access denied.</div>
    <?php else: ?>
    <div class="panel">
        <strong>Accounts (latest 200)</strong>
        <table>
            <thead>
                <tr>
                    <th>ID</th>
                    <th>Email</th>
                    <th>Created</th>
                    <th>Banned</th>
                    <th>Karma</th>
                    <th>State</th>
                    <th>Last Login</th>
                </tr>
            </thead>
            <tbody>
            <?php
                $accounts = $pdo->query(
                    'SELECT ID, email, creation_time, banned, karma, stat_state, login_time ' .
                    'FROM account ORDER BY ID DESC LIMIT 200'
                )->fetchAll();
                foreach ($accounts as $row):
            ?>
                <tr>
                    <td><a href="?page=account&id=<?php echo h($row['ID']); ?>"><?php echo h($row['ID']); ?></a></td>
                    <td><?php echo h($row['email']); ?></td>
                    <td><?php echo h(fmt_time((int)$row['creation_time'])); ?></td>
                    <td><?php echo h($row['banned']); ?></td>
                    <td><?php echo h($row['karma']); ?></td>
                    <td><?php echo h($row['stat_state']); ?></td>
                    <td><?php echo h(fmt_time((int)$row['login_time'])); ?></td>
                </tr>
            <?php endforeach; ?>
            </tbody>
        </table>
    </div>
    <?php endif; ?>
<?php elseif ($page === 'account_create'): ?>
    <div class="panel">
        <strong>Create Account</strong>
        <form method="post" action="?page=account_create">
            <input type="hidden" name="csrf_token" value="<?php echo h(csrf_token()); ?>">
            <div>
                <label>Email</label><br>
                <input type="email" name="email" required>
            </div>
            <div>
                <label>Password</label><br>
                <input type="password" name="password" required>
            </div>
            <button type="submit">Create</button>
        </form>
    </div>
<?php elseif ($page === 'account' && $pdo): ?>
    <?php
        $session_id = (int)($_SESSION['accman_user']['id'] ?? 0);
        $account_id = (int)($_GET['id'] ?? $session_id);
        if ($session_id !== 1 && $account_id !== $session_id) {
            $account_id = 0;
        }
        $stmt = $pdo->prepare('SELECT * FROM account WHERE ID = ?');
        $stmt->execute([$account_id]);
        $account = $stmt->fetch();
    ?>
    <?php if (!$account): ?>
        <div class="error">Access denied.</div>
    <?php else: ?>
        <div class="panel">
            <strong>Account #<?php echo h($account['ID']); ?></strong><br>
            Email: <?php echo h($account['email']); ?><br>
            Created: <?php echo h(fmt_time((int)$account['creation_time'])); ?><br>
            Banned: <?php echo h($account['banned']); ?><br>
            Karma: <?php echo h($account['karma']); ?><br>
            State: <?php echo h($account['stat_state']); ?><br>
            Last Login: <?php echo h(fmt_time((int)$account['login_time'])); ?>
        </div>

        <div class="panel">
            <strong>Characters</strong>
            <table>
                <thead>
                    <tr>
                        <th>ID</th>
                        <th>Name</th>
                        <th>Class</th>
                        <th>Created</th>
                        <th>Last Login</th>
                        <th>Locked</th>
                    </tr>
                </thead>
                <tbody>
                <?php
                    $stmt = $pdo->prepare(
                        'SELECT ID, name, class, creation_time, login_time, locked FROM chars ' .
                        'WHERE sID = ? ORDER BY ID DESC'
                    );
                    $stmt->execute([$account_id]);
                    $chars = $stmt->fetchAll();
                    foreach ($chars as $row):
                ?>
                    <tr>
                        <td><?php echo h($row['ID']); ?></td>
                        <td><?php echo h($row['name']); ?></td>
                        <td><?php echo h(class_label((int)$row['class'])); ?></td>
                        <td><?php echo h(fmt_time((int)$row['creation_time'])); ?></td>
                        <td><?php echo h(fmt_time((int)$row['login_time'])); ?></td>
                        <td><?php echo h($row['locked']); ?></td>
                    </tr>
                <?php endforeach; ?>
                </tbody>
            </table>
        </div>

        <div class="panel">
            <strong>Create Character</strong>
            <?php
                $stmt = $pdo->prepare('SELECT COUNT(*) AS c FROM chars WHERE sID = ?');
                $stmt->execute([$account_id]);
                $char_count = (int)$stmt->fetch()['c'];
            ?>
            <?php if ($char_count >= 20): ?>
                <div class="error">Character limit reached (20).</div>
            <?php else: ?>
                <form method="post" action="?page=character_create">
                    <input type="hidden" name="csrf_token" value="<?php echo h(csrf_token()); ?>">
                    <input type="hidden" name="account_id" value="<?php echo h($account_id); ?>">
                    <div>
                        <label>Name</label><br>
                        <input type="text" name="name" required>
                    </div>
                    <div>
                        <label>Gender</label><br>
                        <select name="gender">
                            <option value="M">Male</option>
                            <option value="F">Female</option>
                        </select>
                    </div>
                    <div>
                        <label>Class</label><br>
                        <select name="class">
                            <option value="W">Warrior</option>
                            <option value="M">Mage</option>
                        </select>
                    </div>
                    <?php if ((int)($_SESSION['accman_user']['id'] ?? 0) === 1): ?>
                        <div>
                            <label>
                                <input type="checkbox" name="is_god" value="1"> God
                            </label>
                        </div>
                    <?php endif; ?>
                    <button type="submit">Create</button>
                </form>
            <?php endif; ?>
        </div>
    <?php endif; ?>
<?php else: ?>
    <div class="error">Unknown page.</div>
<?php endif; ?>

</body>
</html>
