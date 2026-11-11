<?php
// HVSC proxy for the ngSid web player. Same-origin: the browser asks this script, the script asks a
// High Voltage SID Collection mirror, so no CORS is involved.
//   hvsc.php?list=MUSICIANS/H/       directory listing (HTML index page of the mirror)
//   hvsc.php?file=MUSICIANS/H/Hubbard_Rob/Commando.sid     a .sid file
//   hvsc.php?id=97064                the path of a tune by its HVSC database id (plain text)
// Only listings and .sid files are served.

$MIRRORS = [
    'https://hvsc.brona.dk/HVSC/C64Music/',
    'http://hafnium.prg.dtu.dk/HVSC/C64Music/',
];
$MAX = 8 * 1024 * 1024;

function fail(int $code, string $msg): void {
    http_response_code($code);
    header('Content-Type: text/plain; charset=utf-8');
    echo $msg;
    exit;
}

function safe(string $p): bool {
    if ($p === '') return true;
    if ($p[0] === '/' || strpos($p, '\\') !== false || strlen($p) > 600) return false;
    if (preg_match('/[\x00-\x1f]/', $p)) return false;
    foreach (explode('/', $p) as $seg) if ($seg === '..') return false;
    return true;
}

function enc(string $p): string {
    return implode('/', array_map('rawurlencode', explode('/', $p)));
}

function get(string $url, int $max, array $headers = []): ?string {
    if (function_exists('curl_init')) {
        $c = curl_init($url);
        curl_setopt_array($c, [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_FOLLOWLOCATION => true,
            CURLOPT_MAXREDIRS => 3,
            CURLOPT_CONNECTTIMEOUT => 8,
            CURLOPT_TIMEOUT => 25,
            CURLOPT_MAXFILESIZE => $max,
            CURLOPT_USERAGENT => 'ngSid web (+https://github.com/DarkVamp76/ngsid)',
            CURLOPT_HTTPHEADER => $headers,
        ]);
        $r = curl_exec($c);
        $code = curl_getinfo($c, CURLINFO_RESPONSE_CODE);
        curl_close($c);
        return ($r !== false && $code === 200) ? $r : null;
    }
    $ctx = stream_context_create(['http' => ['timeout' => 25, 'user_agent' => 'ngSid web', 'header' => implode("\r\n", $headers)]]);
    $r = @file_get_contents($url, false, $ctx, 0, $max);
    return $r === false ? null : $r;
}

if (isset($_GET['id'])) {
    // Resolve a HVSC database id (hvsc.c64.org/search?sid=ID) to the path inside the collection.
    $id = (string)$_GET['id'];
    if (!preg_match('/^[0-9]{1,9}$/', $id)) fail(400, 'bad id');
    $json = get('https://hvsc.c64.org/api/v1/sids/' . $id, 65536, ['Accept: application/json']);
    $d = $json === null ? null : json_decode($json, true);
    if (!is_array($d) || empty($d['filename']) || !isset($d['filePath'])) fail(404, 'unknown id');
    $path = trim((string)$d['filePath'], '/');
    $path = ($path === '' ? '' : $path . '/') . $d['filename'];
    if (!safe($path) || !preg_match('/\.sid$/i', $path)) fail(404, 'unsupported entry');
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: public, max-age=86400');
    echo $path;
    exit;
}

if (isset($_GET['list'])) {
    $p = (string)$_GET['list'];
    if (!safe($p) || ($p !== '' && substr($p, -1) !== '/')) fail(400, 'bad path');
    $type = 'text/html; charset=utf-8';
    $cache = 600;
} elseif (isset($_GET['file'])) {
    $p = (string)$_GET['file'];
    if (!safe($p) || !preg_match('/\.sid$/i', $p)) fail(400, 'bad path');
    $type = 'application/octet-stream';
    $cache = 86400;
} else {
    fail(400, 'usage: hvsc.php?list=FOLDER/ or hvsc.php?file=PATH.sid');
}

foreach ($MIRRORS as $m) {
    $body = get($m . enc($p), $MAX);
    if ($body !== null) {
        header('Content-Type: ' . $type);
        header('Cache-Control: public, max-age=' . $cache);
        header('X-Content-Type-Options: nosniff');
        echo $body;
        exit;
    }
}
fail(502, 'HVSC mirror unreachable');
