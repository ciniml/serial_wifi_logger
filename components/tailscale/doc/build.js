#!/usr/bin/env node
// Build the Marp slide deck (HTML and/or PDF).
//
// Figures (e.g. demo.svg) are pre-rendered SVG files committed in this
// directory; Marp embeds them as plain images via --allow-local-files.
const { execSync } = require('child_process')
const fs = require('fs')
const path = require('path')

process.chdir(__dirname)

const INPUT = 'presentation.md'
const OUT_HTML = 'presentation.html'
const OUT_PDF = 'presentation.pdf'

const target = (process.argv[2] || 'all').toLowerCase()
if (!['html', 'pdf', 'all'].includes(target)) {
  console.error(`Unknown target: ${target} (expected html | pdf | all)`)
  process.exit(1)
}

if (!fs.existsSync('node_modules')) {
  console.log('Installing node dependencies...')
  execSync('npm install', { stdio: 'inherit' })
}

const marp = (args) =>
  execSync(`npx marp --allow-local-files ${args} ${INPUT}`, { stdio: 'inherit' })

if (target === 'html' || target === 'all') {
  console.log(`Rendering -> ${OUT_HTML}`)
  marp(`-o ${OUT_HTML}`)
  console.log(`Wrote ${path.resolve(OUT_HTML)}`)
}
if (target === 'pdf' || target === 'all') {
  console.log(`Rendering -> ${OUT_PDF}`)
  marp(`--pdf -o ${OUT_PDF}`)
  console.log(`Wrote ${path.resolve(OUT_PDF)}`)
}
