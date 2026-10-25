# VitaSpot GitHub Pages Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an accessible, search-optimized static landing page that presents VitaSpot accurately and directs visitors to its latest release.

**Architecture:** Publish plain HTML, CSS, and JavaScript directly from `docs/`. Keep content semantic and usable without JavaScript; use JavaScript only for navigation and viewport reveals. Reuse the repository’s real screenshots and icon, and generate a local social-sharing image.

**Tech Stack:** HTML5, CSS3, vanilla JavaScript, Python 3 for asset generation and local verification, GitHub Pages.

**Spec:** `docs/superpowers/specs/2026-10-26-github-pages-design.md`

## Global Constraints

No runtime dependencies, analytics, forms, cookies, fabricated proof, or application changes. Canonical URL: `https://0operryo0.github.io/VitaSpot/`. Primary CTA: `https://github.com/0oPerryo0/VitaSpot/releases/latest`. Use the approved typography, spacing, flat-color, motion, copy, and accessibility rules from the design. Preserve the Premium and unofficial-client notices.

## Review Focus

- Project-site URLs must retain `/VitaSpot/` when hosted and still work under a local root server; test desktop and mobile navigation in Task 3.
- Content must remain readable and downloads reachable without JavaScript; test script-disabled behavior in Task 3.
- The latest-release page may not contain a release yet; link to the stable releases-latest route and do not invent a VPK asset URL; verify redirects in Task 3.
- Small screens must not clip the hero, screenshots, navigation, or FAQ; test at 390 px in Task 3.
- Reduced-motion users must see all content immediately; test the media query and reveal fallback in Task 3.

---

### Task 1: Page content, branding, and search metadata

**Files:**
- Create: `docs/index.html`
- Create: `docs/404.html`
- Create: `docs/robots.txt`
- Create: `docs/sitemap.xml`
- Create: `docs/.nojekyll`
- Create: `docs/assets/icon.png`
- Create: `docs/assets/og-image.png`
- Create: `docs/assets/screenshots/{home,playlist,artist,player,player_lyrics}.png`

**Interfaces:**
- Produces: semantic IDs `features`, `install`, `architecture`, and `faq`; asset paths under `docs/assets/`; visible FAQ questions matching FAQ JSON-LD.

- [ ] **Step 1: Check required source assets and links**
Run: `test -f README.md && test -f sce_sys/icon0.png && test -f docs/screenshots/home.png`
Expected: exit 0.

- [ ] **Step 2: Create the semantic landing page**
Implement all sections in the approved outline, with one H1, descriptive alt text, a skip link, native FAQ details, real repository links, and visible Premium/unofficial notices. Use root-relative asset and fragment links so local preview works; use absolute canonical and sitemap URLs.

- [ ] **Step 3: Add indexing and support files**
Add title/description, canonical, Open Graph/Twitter metadata, SoftwareApplication and FAQPage JSON-LD, robots, sitemap, `.nojekyll`, favicon, and branded 404. Copy only existing images and generate the 1200×630 social card locally without external assets.

- [ ] **Step 4: Verify content and metadata**
Run a Python HTMLParser check for one H1, required IDs, image alt attributes, canonical URL, valid JSON-LD parsing, and FAQ text agreement.
Expected: all assertions pass.

### Task 2: Responsive visual system and progressive interactions

**Files:**
- Create: `docs/assets/site.css`
- Create: `docs/assets/site.js`
- Modify: `docs/index.html`

**Interfaces:**
- Consumes: Task 1 semantic sections and assets.
- Produces: `[data-reveal]`, `[data-tagline]`, `nav-toggle`, `nav-menu`; CSS classes `.js`, `.is-visible`, `.menu-open`.

- [ ] **Step 1: Build the section-by-section visual system**
Style hero, features, tagline, installation, architecture, FAQ, final CTA, and footer in that order. Follow the approved type/spacing tokens, dark flat backgrounds, green accents, device-inspired screenshot framing, full states, and mobile breakpoints.

- [ ] **Step 2: Implement progressive navigation and reveals**
Use IntersectionObserver for section and word reveals. Implement the menu-button morph, glass overlay, Escape dismissal, link dismissal, and scroll locking. Add `.js` early, but ensure `.no-js`/missing-script content remains visible and `prefers-reduced-motion` removes nonessential motion.

- [ ] **Step 3: Run static behavior checks**
Run: `node --check docs/assets/site.js` and a CSS/HTML reference check for every local `src`, `href`, and `url()` asset.
Expected: valid JavaScript and no missing local files.

### Task 3: End-to-end browser and delivery verification

**Files:**
- Modify only files needing fixes from Tasks 1–2.

**Interfaces:**
- Consumes: the complete static site.

- [ ] **Step 1: Start a local site**
Run: `python3 -m http.server 8000 --directory docs`
Expected: `http://127.0.0.1:8000/` returns the landing page.

- [ ] **Step 2: Verify desktop behavior**
In the built-in browser at 1440×900, inspect the hero and every section, click navigation and FAQ controls, check the download URL, and confirm no browser console errors or failed local requests.
Expected: faithful design, working interactions, zero local 404s.

- [ ] **Step 3: Verify mobile and resilient states**
At 390×844, inspect all sections and exercise open/close/Escape navigation. Check keyboard focus, reduced motion, and script-disabled content visibility. Request a missing URL to inspect the branded 404.
Expected: no horizontal overflow, hidden content, dead local links, or inaccessible menu behavior.

- [ ] **Step 4: Run final automated checks**
Repeat metadata, JSON-LD, local-asset, JavaScript syntax, and Git diff checks. Confirm no application source files changed.
Expected: all checks pass; changes are confined to `docs/`.
