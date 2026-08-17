// Which of the two themes is showing, and why.
//
// daisyUI's own `theme-controller` is a checkbox and a `:has()` selector, which
// is elegant and forgets the choice on every reload. A console is a tab that
// stays open for days and gets reloaded after every deploy; a preference that
// does not survive that is not a preference. So the attribute is written to the
// document and mirrored into localStorage, and `app.html` replays it before the
// first paint.

const STORAGE_KEY = 'mb-theme';

export const THEMES = {
	light: 'monobucket',
	dark: 'monobucket-dark'
} as const;

/// `system` is the absence of a stored choice, not a third theme: it defers to
/// the `prefersdark` media query in the stylesheet, which is what a reader who
/// has never touched the control should get.
export type ThemeChoice = 'system' | 'light' | 'dark';

function read(): ThemeChoice {
	if (typeof document === 'undefined') return 'system';
	const attribute = document.documentElement.dataset.theme;
	if (attribute === THEMES.dark) return 'dark';
	if (attribute === THEMES.light) return 'light';
	return 'system';
}

class Theme {
	choice = $state<ThemeChoice>(read());

	set(next: ThemeChoice) {
		this.choice = next;

		if (next === 'system') {
			delete document.documentElement.dataset.theme;
		} else {
			document.documentElement.dataset.theme = THEMES[next];
		}

		try {
			if (next === 'system') localStorage.removeItem(STORAGE_KEY);
			else localStorage.setItem(STORAGE_KEY, THEMES[next]);
		} catch {
			// The theme still applies for this session; it just will not survive
			// a reload. Not worth telling anyone about.
		}
	}
}

export const theme = new Theme();
