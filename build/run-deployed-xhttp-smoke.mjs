// Fetch the scoped database credential directly into child-process memory;
// never print it or persist it in a shell script/config file.
import {execFileSync, spawn} from 'node:child_process';
const raw = execFileSync('kubectl', ['--context', 'k8s-nj-prod', '-n', 'byod', 'get',
  'secret', 'byod-server-runtime', '-o', 'json'], {encoding: 'utf8'});
const secret = JSON.parse(raw);
const database = new URL(Buffer.from(secret.data['database-url'], 'base64').toString('utf8'));
database.hostname = '127.0.0.1';
database.port = '55438'; // Loopback-only kubectl port-forward, never public.
const child = spawn('go', ['test', '-run', '^TestDeployedXHTTP$', '-count=1', '-v', '-timeout', '3m', '.'], {
  cwd: new URL('../byod_server/', import.meta.url),
  stdio: 'inherit',
  env: {...process.env, BYOD_LIVE_SMOKE_DATABASE_URL: database.toString(),
    BYOD_LIVE_SMOKE_ENDPOINT: 'https://exam.cs.ac.cn'},
});
child.on('exit', code => {process.exitCode = code ?? 1;});
