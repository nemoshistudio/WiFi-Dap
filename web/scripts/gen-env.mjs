import { execFileSync, spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/**
 * Build metadata injector (docs/versioning.md §4.2).
 *
 * - VITE_APP_WEB_VERSION comes from web/package.json (single source, required).
 * - VITE_APP_GIT_TAG / VITE_APP_GIT_COMMIT are provenance only; all git calls
 *   fall back instead of throwing so shallow/source builds still work.
 * - VITE_APP_LAST_COMMIT keeps its historical meaning (last commit date) for
 *   the existing About page.
 */

const webRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const DEFAULT_GIT_TAG = 'v1.0.0-unknown';
const DEFAULT_GIT_COMMIT = 'unknown';
const DEFAULT_LAST_COMMIT = 'unknown';
const SEMVER_RE = /^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/;

function warn(message) {
    console.warn(`[gen-env] WARNING: ${message}`);
}

function readWebVersion() {
    const packageFile = resolve(webRoot, 'package.json');
    let pkg;
    try {
        pkg = JSON.parse(readFileSync(packageFile, 'utf8'));
    } catch (error) {
        throw new Error(`[gen-env] cannot read ${packageFile}: ${error.message}`);
    }
    const version = pkg?.version;
    if (typeof version !== 'string' || !SEMVER_RE.test(version)) {
        throw new Error(
            `[gen-env] invalid package.json version "${String(version)}" (expected MAJOR.MINOR.PATCH)`,
        );
    }
    return version;
}

function git(args, fallback, label) {
    try {
        const out = execFileSync('git', args, {
            cwd: webRoot,
            encoding: 'utf8',
            stdio: ['ignore', 'pipe', 'ignore'],
        }).trim();
        if (out) {
            return out;
        }
        warn(`${label} returned empty output, falling back to ${fallback}`);
    } catch {
        warn(`${label} unavailable, falling back to ${fallback}`);
    }
    return fallback;
}

const webVersion = readWebVersion();
const gitTag = git(
    ['describe', '--tags', '--dirty', '--always', '--match', 'v[0-9]*'],
    DEFAULT_GIT_TAG,
    'git describe',
);
const gitCommit = git(['rev-parse', '--short', 'HEAD'], DEFAULT_GIT_COMMIT, 'git rev-parse');
const lastCommit = git(['log', '-1', '--format=%cd'], DEFAULT_LAST_COMMIT, 'git log');

function readProtocolVersion() {
    try {
        const source = readFileSync(resolve(webRoot, 'src/api/index.ts'), 'utf8');
        const match = source.match(/export const PROTOCOL_VERSION\s*=\s*(\d+)/);
        return match ? Number.parseInt(match[1], 10) : 1;
    } catch (error) {
        warn(`cannot read PROTOCOL_VERSION from src/api/index.ts: ${error.message}`);
        return 1;
    }
}

const protocolVersion = readProtocolVersion();

console.log(
    `[gen-env] web=${webVersion} tag=${gitTag} commit=${gitCommit} lastCommit=${lastCommit} protocol=${protocolVersion}`,
);

/**
 * docs/versioning.md §4.2-3：构建结束后把构建信息写到 web/dist/build-info.json，
 * 供 About 页/自动化验收读取（不参与固件嵌入）。
 */
function writeBuildInfo() {
    try {
        const outDir = resolve(webRoot, process.env.VITE_OUTPUT_DIR || 'dist');
        mkdirSync(outDir, { recursive: true });
        const info = {
            web_version: webVersion,
            protocol_version: protocolVersion,
            git_tag: gitTag,
            git_commit: gitCommit,
            last_commit: lastCommit,
            built_at: new Date().toISOString(),
        };
        writeFileSync(resolve(outDir, 'build-info.json'), `${JSON.stringify(info, null, 2)}\n`);
        console.log(`[gen-env] wrote ${resolve(outDir, 'build-info.json')}`);
    } catch (error) {
        warn(`cannot write dist/build-info.json: ${error.message}`);
    }
}

// run whatever came after "npm run dev/build/preview ...", e.g. ["--host"]
const argsFromNpm = process.argv.slice(2);
const localVite = resolve(webRoot, 'node_modules/vite/bin/vite.js');
const command = existsSync(localVite)
    ? { file: process.execPath, args: [localVite, ...argsFromNpm] }
    : {
          file: process.platform === 'win32' ? 'npx.cmd' : 'npx',
          args: ['vite', ...argsFromNpm],
      };

const child = spawn(command.file, command.args, {
    cwd: webRoot,
    stdio: 'inherit',
    env: {
        ...process.env,
        VITE_APP_WEB_VERSION: webVersion,
    },
});

child.on('error', (error) => {
    console.error(`[gen-env] failed to start vite: ${error.message}`);
    process.exit(1);
});

child.on('exit', (code, signal) => {
    if (signal) {
        // Mirror the child's termination (Ctrl-C etc.) instead of exiting 0.
        process.kill(process.pid, signal);
        return;
    }
    if (code === 0 && argsFromNpm.includes('build')) {
        writeBuildInfo();
    }
    process.exit(code ?? 1);
});
