#!/bin/bash
set -eu

(deno run --allow-read - > bin/deno-output) <<'EOJS'
import { TextLineStream } from 'https://deno.land/std/streams/mod.ts'

const file = await Deno.readTextFile("regex_tests.txt");
const lines = file.split('\n');

let regex = null;

for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
    const line = lines[lineIndex];

    if (line[0] == '#')  continue;

    if (line.length == 0) {
        regex = null;
        continue;
    }

    if (!regex) {
        if (line[0] != '/' || line[line.length-1] != '/')  throw new Error("Expected regex to start and end with '/'.");

        const anchored = `^(${line.slice(1, line.length-1)})$`;

        regex = new RegExp(anchored);

        continue;
    }

    const match = line.match(regex);

    let output = `Regex:  ${regex.source.slice(2, regex.source.length-2)}\n`;
    output    += `String: ${line}\n`;
    output    += `Match:  ${match ? "yes" : "no"}\n`;
    if (match) {
        for (let i = 2; i < match.length; i++) {
            output += `  ${i-2}: `;
            if (match[i]!==undefined)  output += match[i];
            else                       output += "(null)"; // This means our output distinguishes between "nothing captured" and "empty string captured".
            output += `\n`;
        }
        let groups = match.groups || {};
        for (const key of Object.keys(groups)) {
            if (groups[key]!==undefined)  output += `  ${key}: ${groups[key]}\n`;
        }
    }
    console.log(output);
}
EOJS

make > /dev/null
bin/scripts/regex-test > bin/our-output

git diff --no-index -- bin/{our,deno}-output
