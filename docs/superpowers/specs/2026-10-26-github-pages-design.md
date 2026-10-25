# VitaSpot GitHub Pages design

## Intent

Create a polished project landing page that helps PS Vita owners understand VitaSpot, see the real interface, and download the latest release. Search visitors should be able to quickly determine what the app does, what it requires, and how to install it.

## Goals, non-goals, and anti-goals

Goals: one clear download action; accurate project copy; real application screenshots; responsive and accessible presentation; complete installation guidance; indexable feature and FAQ content; no build dependencies.

Non-goals: account creation, analytics, advertising, a web music player, fabricated testimonials or usage metrics, and changing the VitaSpot application.

Anti-goals: generic open source template styling, unsupported claims, hiding the Spotify Premium requirement or unofficial status, dead controls, heavy JavaScript, and a deployment process that requires local tooling.

## Constraints

The site lives in `docs/` and publishes from `main` using GitHub Pages. It uses static HTML, CSS, and JavaScript with no framework, package installation, cookies, forms, or external runtime dependencies. Repository URLs use `0oPerryo0/VitaSpot`; the canonical site is `https://0operryo0.github.io/VitaSpot/`. The latest-release URL is `https://github.com/0oPerryo0/VitaSpot/releases/latest`.

## Approaches

A custom static site was selected for complete design control, strong performance, and direct publishing. Jekyll was considered for future documentation growth but adds unneeded templates and configuration. A framework and Actions build was rejected as unnecessary maintenance for one landing page.

## Page outline and copy direction

1. Floating navigation with feature, installation, FAQ, and source links.
2. Hero: “Your music. Back on your PS Vita.” Supporting copy identifies an unofficial Spotify client with native playback and Spotify Connect. Primary CTA: “Download VitaSpot”. Proof line: “320 kbps audio · Gapless playback · Synced lyrics”. The real Home screenshot appears in a Vita-inspired device frame.
3. Feature section with four outcome-led items: native listening, library and search, synced lyrics, and controls designed for a five-inch screen. Actual screenshots provide product proof.
4. Large tagline reveal: “A second life for the console you never stopped loving.” Words reveal in reading order as the section enters view.
5. Installation in three steps: install the VPK with VitaShell, connect both devices to the same Wi-Fi, then choose PS Vita in Spotify’s device picker. A details section covers the PC credential fallback and links to complete build instructions.
6. Technical architecture section explaining the non-blocking C11/SDL2 frontend, plain C backend contract, and Rust/librespot backend. Include the mock backend for UI development.
7. FAQ covering Premium, official status, supported playback, sign-in, controls, lyrics, source/building, and where data is stored.
8. Final download CTA and an honest risk notice. No account, payment, tracking, or email is required by this website.
9. Footer with repository, releases, issues, README, and license/credits documentation. Legal text identifies the project as unofficial and personal-use only rather than inventing separate privacy or terms pages.

## Visual and interaction system

Use a dark, flat background with restrained Spotify-green accents and actual application imagery. Use Manrope from a self-hosted asset if available, with a system sans fallback; no italics or ultra-bold type. Typography follows the specified 12–96 px scale and spacing follows the supplied 2–96 px tokens. Rounded screen cards and a floating glass navigation pill establish the music-product feel. Icons are locally inlined from Phosphor’s visual style, not a runtime icon dependency.

Motion uses the custom `cubic-bezier(0.32,0.72,0,1)` easing. Sections reveal with IntersectionObserver, navigation expands into a glass overlay with staggered links and a morphing menu button, and the tagline activates word by word. `prefers-reduced-motion` disables nonessential movement. JavaScript is progressive enhancement: content, installation, FAQ, and download remain usable without it.

Every link has hover, active, and keyboard focus states. Include a skip link, semantic landmarks, descriptive alt text, sufficient contrast, visible current-page navigation, and no empty or loading interface states because the site has no remote application data.

## SEO, AEO, and sharing

Index the evergreen landing page. Title: “VitaSpot — Spotify on PS Vita | Unofficial Homebrew Client”. Description: “Play Spotify on your PS Vita with VitaSpot, an unofficial homebrew client featuring Spotify Connect, synced lyrics, gapless playback, search, and touch controls.”

Use canonical, robots, Open Graph, Twitter card, theme color, favicon, and a purpose-built 1200×630 social image. Add JSON-LD for SoftwareApplication and FAQPage using only visible, accurate content. Add `sitemap.xml`, `robots.txt`, `.nojekyll`, and a branded `404.html` that works at the repository project path. Preserve descriptive headings and plain question-and-answer text for answer engines.

## Files

- `docs/index.html`: semantic content and metadata
- `docs/assets/site.css`: responsive visual system and states
- `docs/assets/site.js`: mobile navigation and progressive reveal behavior
- `docs/assets/screenshots/`: copied real project screenshots
- `docs/assets/icon.png` and `docs/assets/og-image.png`: branding and sharing
- `docs/404.html`, `docs/robots.txt`, `docs/sitemap.xml`, `docs/.nojekyll`: Pages support

## Verification

Serve `docs/` locally and inspect desktop and mobile rendering in a browser. Exercise navigation, FAQ, download/source links, reduced motion, keyboard focus, and the 404 page. Check HTML metadata and structured data, confirm every local asset resolves, and confirm no JavaScript errors occur. Existing application tests are unchanged; the standard C build is not a meaningful test of this static site.

## Builder discretion

Fine-tune screenshot cropping, responsive composition, exact copy lengths, and decorative CSS geometry within the visual rules. Do not introduce claims, dependencies, analytics, fake social proof, or new product requirements.
