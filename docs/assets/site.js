(() => {
  const toggle = document.getElementById('nav-toggle');
  const menu = document.getElementById('nav-menu');
  const main = document.querySelector('main');
  const footer = document.querySelector('footer');
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const revealItems = document.querySelectorAll('[data-reveal]');
  const taglines = document.querySelectorAll('[data-tagline]');

  taglines.forEach((tagline) => {
    const words = tagline.textContent.trim().split(/\s+/);
    tagline.textContent = '';
    words.forEach((word, index) => {
      const span = document.createElement('span');
      span.className = 'word';
      span.textContent = word;
      span.style.transitionDelay = reducedMotion.matches ? '0ms' : `${index * 90}ms`;
      tagline.append(span, document.createTextNode(' '));
    });
  });

  if (!('IntersectionObserver' in window) || reducedMotion.matches) {
    revealItems.forEach((item) => item.classList.add('is-visible'));
    document.querySelectorAll('.word').forEach((word) => word.classList.add('is-visible'));
  } else {
    const revealObserver = new IntersectionObserver((entries, observer) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          entry.target.classList.add('is-visible');
          observer.unobserve(entry.target);
        }
      });
    }, { threshold: 0.12, rootMargin: '0px 0px -48px' });
    revealItems.forEach((item) => revealObserver.observe(item));

    const wordObserver = new IntersectionObserver((entries, observer) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          entry.target.classList.add('is-visible');
          observer.unobserve(entry.target);
        }
      });
    }, { threshold: 0.8, rootMargin: '0px 0px -15% 0px' });
    document.querySelectorAll('.word').forEach((word) => wordObserver.observe(word));
  }

  const setBackgroundInert = (inert) => {
    [main, footer].forEach((element) => {
      element.inert = inert;
      if (inert) element.setAttribute('aria-hidden', 'true');
      else element.removeAttribute('aria-hidden');
    });
  };

  const closeMenu = () => {
    toggle.setAttribute('aria-expanded', 'false');
    toggle.setAttribute('aria-label', 'Open navigation');
    menu.classList.remove('is-open');
    document.body.classList.remove('menu-open');
    setBackgroundInert(false);
  };

  toggle.addEventListener('click', () => {
    const open = toggle.getAttribute('aria-expanded') !== 'true';
    if (!open) {
      closeMenu();
      return;
    }
    toggle.setAttribute('aria-expanded', 'true');
    toggle.setAttribute('aria-label', 'Close navigation');
    menu.classList.add('is-open');
    document.body.classList.add('menu-open');
    setBackgroundInert(true);
    menu.querySelector('a').focus();
  });

  menu.querySelectorAll('a').forEach((link) => link.addEventListener('click', closeMenu));
  document.addEventListener('keydown', (event) => {
    if (toggle.getAttribute('aria-expanded') !== 'true') return;
    if (event.key === 'Escape') {
      closeMenu();
      toggle.focus();
    }
    if (event.key === 'Tab') {
      const focusable = [...document.querySelectorAll('.nav-shell a, .nav-shell button')];
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    }
  });
  window.matchMedia('(min-width: 801px)').addEventListener('change', (event) => {
    if (event.matches) closeMenu();
  });

  document.documentElement.classList.replace('no-js', 'js');
})();
