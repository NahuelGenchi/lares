#!/usr/bin/env node
// The privacy gate: fails when a tracked file, a commit or a built program
// holds a private detail. Node alone, no packages.
//
//   node tools/privacy-gate.mjs                      every tracked file
//   node tools/privacy-gate.mjs --commits A..B       messages and identities
//   node tools/privacy-gate.mjs --binaries DIR...    strings of built programs
//   node tools/privacy-gate.mjs --files FILE...      the given files, as tracked
//   node tools/privacy-gate.mjs --self-test          the rules on made-up samples
//
// What it looks for:
//   user-folder      a Windows, Linux or macOS user folder with a real name
//   private-address  a private, carrier-grade NAT (Tailscale) or Tailscale IPv6 address
//   email            an e-mail address that isn't a no-reply or example one
//   secret           common token and private key shapes
//   banned-name      a product name that must never appear here
//   builder          (programs only) this machine's account or computer name
//
// Exceptions live in tools/privacy-allow.txt, one per line: rule, path
// pattern (* matches anything), exact value (* for any). Add one only for a value that is
// public by nature, such as an upstream author's address in a licence header.

import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { hostname, userInfo } from 'node:os';
import { dirname, extname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');

// Folder names that stand for "whoever you are", not for a person.
const PLACEHOLDER_USERS = new Set([
    'user', 'username', 'name', 'you', 'me', 'public', 'default', 'shared',
    'all users', 'example', 'someone', 'runner', 'runneradmin',
]);

const USER_FOLDER =
    /(?<![A-Za-z0-9_-]\.[A-Za-z]{2,10})(?:[A-Za-z]:)?[\\/]{1,2}(?:Users|home|Documents and Settings)[\\/]{1,2}([^\\/\s"'`<>|:*?,;()[\]{}]+)/gi;
const IPV4 = /(?<![\d.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?!\.?\d)/g;
const TAILSCALE_V6 = /\bfd7a:115c:a1e0(?::[0-9a-f]{0,4}){1,5}/gi;
const EMAIL = /[A-Za-z0-9][A-Za-z0-9._%+-]*@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}/g;
const SECRETS = [
    /\bgh[pousr]_[A-Za-z0-9]{36,}/g,
    /\bgithub_pat_[A-Za-z0-9_]{22,}/g,
    /\bAKIA[0-9A-Z]{16}\b/g,
    /-----BEGIN [A-Z ]*PRIVATE KEY-----/g,
    /\bxox[abprs]-[A-Za-z0-9-]{10,}/g,
    /\btskey-[a-z]+-[A-Za-z0-9-]{8,}/g,
    /\bAIza[0-9A-Za-z_-]{35}\b/g,
    /\bsk-(?:ant-|proj-)?[A-Za-z0-9_-]{32,}/g,
    /\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}/g,
];
// Written so that this file is no match for it.
const BANNED = /see[l]en/gi;

function privateV4(a, b, c, d) {
    if ([a, b, c, d].some((n) => n > 255)) return false;
    if (a === 10) return true;
    if (a === 172 && b >= 16 && b <= 31) return true;
    if (a === 192 && b === 168) return true;
    if (a === 100 && b >= 64 && b <= 127) return true;
    return false;
}

function emailAllowed(address) {
    const at = address.lastIndexOf('@');
    const local = address.slice(0, at).toLowerCase();
    const domain = address.slice(at + 1).toLowerCase();
    if (/^(no-?reply|do-?not-?reply)\b/.test(local)) return true;
    if (domain === 'users.noreply.github.com') return true;
    if (/(^|\.)example\.(com|org|net)$/.test(domain)) return true;
    if (/\.(example|invalid|test|localhost)$/.test(domain)) return true;
    if (/\.(png|jpe?g|gif|svg|webp|ico|bmp)$/.test(domain)) return true; // icon@2x.png
    return false;
}

// Every finding in one piece of text, as { rule, value }.
function findings(text) {
    const found = [];
    for (const m of text.matchAll(USER_FOLDER)) {
        const name = m[1];
        if (/^[<%${~.]/.test(name) || PLACEHOLDER_USERS.has(name.toLowerCase())) continue;
        found.push({ rule: 'user-folder', value: m[0] });
    }
    for (const m of text.matchAll(IPV4)) {
        if (privateV4(+m[1], +m[2], +m[3], +m[4])) found.push({ rule: 'private-address', value: m[0] });
    }
    for (const m of text.matchAll(TAILSCALE_V6)) found.push({ rule: 'private-address', value: m[0] });
    for (const m of text.matchAll(EMAIL)) {
        if (!emailAllowed(m[0])) found.push({ rule: 'email', value: m[0] });
    }
    for (const pattern of SECRETS) {
        for (const m of text.matchAll(pattern)) found.push({ rule: 'secret', value: m[0] });
    }
    for (const m of text.matchAll(BANNED)) found.push({ rule: 'banned-name', value: m[0] });
    return found;
}

function loadAllowList() {
    const file = join(HERE, 'privacy-allow.txt');
    if (!existsSync(file)) return [];
    const entries = [];
    for (const raw of readFileSync(file, 'utf8').split(/\r?\n/)) {
        const line = raw.trim();
        if (!line || line.startsWith('#')) continue;
        const m = line.match(/^(\S+)\s+(\S+)\s+(.+)$/);
        if (!m) throw new Error(`privacy-allow.txt: can't read "${line}"`);
        const glob = m[2].replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*');
        entries.push({ rule: m[1], path: new RegExp(`^${glob}$`), value: m[3] });
    }
    return entries;
}

const allowList = loadAllowList();

function allowed(rule, path, value) {
    return allowList.some((e) => e.rule === rule && (e.value === '*' || e.value === value) && e.path.test(path));
}

// A token is never printed whole, and the banned name never at all: the
// output of a failed run is public too.
function shown(rule, value) {
    if (rule === 'banned-name' && value !== 'in the file name') return 'a name that must not appear in Lares';
    if (rule === 'secret') return `${value.slice(0, 10)}... (${value.length} characters)`;
    return value;
}

const problems = [];

function report(rule, where, value) {
    problems.push(`${where}: ${rule}: ${shown(rule, value)}`);
}

function isBinary(buffer) {
    return buffer.subarray(0, 8192).includes(0);
}

// Printable runs in a program: 8-bit text and UTF-16LE text, five characters or more.
function strings(buffer) {
    const out = [];
    const latin = buffer.toString('latin1');
    for (const m of latin.matchAll(/[\x20-\x7e]{5,}/g)) out.push(m[0]);
    for (const m of latin.matchAll(/(?:[\x20-\x7e]\x00){5,}/g)) out.push(m[0].replace(/\x00/g, ''));
    return out;
}

function scanText(path, text) {
    const lines = text.split(/\r?\n/);
    lines.forEach((line, i) => {
        for (const f of findings(line)) {
            if (!allowed(f.rule, path, f.value)) report(f.rule, `${path}:${i + 1}`, f.value);
        }
    });
}

function scanStrings(path, buffer, extra = []) {
    const seen = new Set();
    for (const s of strings(buffer)) {
        for (const f of findings(s)) {
            const key = `${f.rule}\0${f.value}`;
            if (seen.has(key) || allowed(f.rule, path, f.value)) continue;
            seen.add(key);
            report(f.rule, path, f.value);
        }
        const lower = s.toLowerCase();
        for (const e of extra) {
            if (lower.includes(e.needle) && !seen.has(e.label)) {
                seen.add(e.label);
                report('builder', path, e.label);
            }
        }
    }
}

function scanTracked(paths) {
    for (const path of paths) {
        const shownPath = path.replace(/\\/g, '/');
        for (const f of findings(shownPath)) {
            if (f.rule === 'banned-name') report(f.rule, shownPath, 'in the file name');
        }
        let buffer;
        try {
            buffer = readFileSync(join(ROOT, path));
        } catch {
            continue; // deleted in the working tree, or a submodule
        }
        if (isBinary(buffer)) scanStrings(shownPath, buffer);
        else scanText(shownPath, buffer.toString('utf8'));
    }
    return paths.length;
}

function git(args) {
    return execFileSync('git', args, { cwd: ROOT, encoding: 'utf8', maxBuffer: 256 << 20 });
}

function scanCommits(range) {
    const log = git(['log', '--format=%H%x00%an%x00%ae%x00%cn%x00%ce%x00%B%x01', range]);
    let count = 0;
    for (const entry of log.split('\x01')) {
        const fields = entry.replace(/^\s+/, '').split('\0');
        if (fields.length < 6) continue;
        count++;
        const [hash, authorName, authorMail, committerName, committerMail, body] = fields;
        const where = `commit ${hash.slice(0, 9)}`;
        for (const [role, mail] of [['author', authorMail], ['committer', committerMail]]) {
            if (!emailAllowed(mail)) report('email', where, `the ${role}'s address is not a no-reply one`);
        }
        const text = `${authorName}\n${committerName}\n${body}`;
        for (const f of findings(text)) {
            if (!allowed(f.rule, 'commit', f.value)) report(f.rule, where, f.value);
        }
    }
    return count;
}

const PROGRAM_TYPES = new Set(['.exe', '.dll', '.pdb', '.sys', '.msi', '.msix', '.appx']);

function programsUnder(target) {
    const full = resolve(target);
    if (!statSync(full).isDirectory()) return [full];
    const out = [];
    for (const entry of readdirSync(full, { withFileTypes: true })) {
        const child = join(full, entry.name);
        if (entry.isDirectory()) out.push(...programsUnder(child));
        else if (PROGRAM_TYPES.has(extname(entry.name).toLowerCase())) out.push(child);
    }
    return out;
}

// This machine's account and computer name, which no program may carry.
// Names too short or too plain to tell apart from ordinary text are skipped.
function builderNeedles() {
    const needles = [];
    const add = (value, label) => {
        if (!value) return;
        const needle = String(value).toLowerCase();
        if (needle.length < 4 || PLACEHOLDER_USERS.has(needle) || needle === 'administrator') return;
        needles.push({ needle, label });
    };
    try {
        add(userInfo().username, "the builder's account name");
    } catch {
        // no account database entry; the environment still says
    }
    add(process.env.USERNAME || process.env.USER, "the builder's account name");
    add(hostname(), "the builder's computer name");
    add(process.env.COMPUTERNAME, "the builder's computer name");
    return needles;
}

function scanBinaries(targets) {
    const extra = builderNeedles();
    const files = targets.flatMap(programsUnder);
    if (!files.length) throw new Error('no built programs found to check');
    for (const file of files) {
        // Reported by file name only: the folder it was built in is the builder's.
        scanStrings(file.replace(/\\/g, '/').split('/').pop(), readFileSync(file), extra);
    }
    return files.length;
}

// The samples are put together here so that no line of this file is itself a
// finding.
function selfTest() {
    const j = (...parts) => parts.join('');
    const mustFail = {
        'user-folder': [
            j('C:', '\\Users\\', 'jsmith', '\\Desktop\\notes.txt'),
            j('c:', '/users/', 'jsmith', '/x'),
            j('C:', '\\\\Users\\\\', 'jsmith', '\\\\x'),
            j('/home/', 'jsmith', '/.config/lares'),
            j('/mnt/c/Users/', 'jsmith', '/x'),
            j('/Users/', 'jsmith', '/Library'),
        ],
        'private-address': [
            j('192.168', '.1.20'), j('10.0', '.0.7'), j('172.16', '.4.1'), j('100.101', '.102.103'),
            j('fd7a:115c', ':a1e0::1234'),
        ],
        email: [j('jane.doe', '@', 'gmail.com'), j('me', '@', 'my-company.co.uk')],
        secret: [
            j('ghp', '_', 'a'.repeat(36)),
            j('github', '_pat_', 'A1'.repeat(20)),
            j('AKIA', 'ABCDEFGHIJKLMNOP'),
            j('-----BEGIN', ' RSA PRIVATE', ' KEY-----'),
            j('-----BEGIN', ' PRIVATE', ' KEY-----'),
            j('tskey', '-auth-', 'kAbCdEf123456'),
            j('xoxb', '-123456789012-abcdef'),
        ],
        'banned-name': [j('See', 'len'), j('SEE', 'LEN'), j('see', 'len-ui')],
    };
    const mustPass = [
        'C:\\Users\\<name>\\AppData', '%USERPROFILE%\\Documents', '/home/you/.local', '~/.local/bin',
        'C:\\Users\\Public\\Desktop', 'https://github.com/users/someone',
        '127.0.0.1', '0.0.0.0', '8.8.8.8', '10.0.26100.1', 'version 1.2.3.4', '255.255.255.0',
        j('someone', '@', 'users.noreply.github.com'), j('noreply', '@', 'github.com'),
        j('jane', '@', 'example.com'), 'icon@2x.png', 'seen', 'unseen', 'seeled',
    ];
    let bad = 0;
    for (const [rule, samples] of Object.entries(mustFail)) {
        for (const sample of samples) {
            if (!findings(sample).some((f) => f.rule === rule)) {
                console.error(`self-test: ${rule} missed a sample of ${sample.length} characters`);
                bad++;
            }
        }
    }
    for (const sample of mustPass) {
        const got = findings(sample);
        if (got.length) {
            console.error(`self-test: "${sample}" was taken for ${got[0].rule}`);
            bad++;
        }
    }
    // The same samples as a program would hold them.
    const wide = Buffer.from(`\0\0${mustFail['user-folder'][0]}\0\0`, 'utf16le');
    const narrow = Buffer.from(`\x01\x02${mustFail.email[0]}\x00\x03`, 'latin1');
    for (const buffer of [wide, narrow]) {
        if (!strings(buffer).some((s) => findings(s).length)) {
            console.error('self-test: a sample inside a program was missed');
            bad++;
        }
    }
    if (bad) process.exit(1);
    console.log('privacy gate: self-test passed');
}

function main() {
    const args = process.argv.slice(2);
    let what;
    if (args[0] === '--self-test') {
        selfTest();
        return;
    }
    if (args[0] === '--commits') {
        if (!args[1]) throw new Error('--commits needs a range, such as origin/main..HEAD');
        what = `${scanCommits(args[1])} commits`;
    } else if (args[0] === '--binaries') {
        if (args.length < 2) throw new Error('--binaries needs a folder or files');
        what = `${scanBinaries(args.slice(1))} built programs`;
    } else if (args[0] === '--files') {
        what = `${scanTracked(args.slice(1))} files`;
    } else if (args.length === 0) {
        const paths = git(['ls-files', '-z']).split('\0').filter(Boolean);
        what = `${scanTracked(paths)} tracked files`;
    } else {
        throw new Error(`unknown option ${args[0]}`);
    }
    if (problems.length) {
        for (const p of problems) console.error(p);
        console.error(`privacy gate: ${problems.length} private details in ${what}. ` +
            'Remove them, or see tools/privacy-allow.txt for values that are public by nature.');
        process.exit(1);
    }
    console.log(`privacy gate: ${what} checked, nothing private found`);
}

try {
    main();
} catch (error) {
    console.error(`privacy gate: ${error.message}`);
    process.exit(2);
}
