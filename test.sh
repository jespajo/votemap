#!/bin/bash

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

        regex = new RegExp(line.slice(1, line.length-1));
        continue;
    }

    const match = line.match(regex);

    let output = `Regex:  ${regex.source}\n`;
    output    += `String: ${line}\n`;
    output    += `Match:  ${match ? "yes" : "no"}\n`;
    if (match) {
        for (let i = 1; i < match.length; i++)  output += `  ${match[i]}\n`;
    }

    console.log(output);
}
EOJS

bin/main > bin/our-output

git diff --no-index -- bin/{deno,our}-output
