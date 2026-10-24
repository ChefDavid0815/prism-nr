import { execFile } from 'node:child_process';

/**
 * Which of these executable names are running right now (tasklist, any case). The community uses it
 * to put the game being played first: a look imported then reaches it within a second, because Prism
 * Core reads prism-core.ini again whenever the file changes.
 */
export function runningExes(names: string[]): Promise<string[]> {
  if (!names.length) return Promise.resolve([]);
  return new Promise((resolve) => {
    execFile('tasklist', ['/fo', 'csv', '/nh'], { windowsHide: true, maxBuffer: 16 << 20 }, (err, out) => {
      if (err) return resolve([]);
      const live = new Set(out.split(/\r?\n/).map((line) => line.split('","')[0].replace(/^"/, '').toLowerCase()).filter(Boolean));
      resolve(names.filter((n) => live.has(n.toLowerCase())));
    });
  });
}
