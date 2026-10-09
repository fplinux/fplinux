export interface SearchPage {
	text: string;
	title: string;
	url: string;
}
declare global {
	interface Window {
		fplinuxOfflineIndex: SearchPage[];
	}
}

class OfflineSiteSearch extends HTMLElement {
	connectedCallback() {
		const opener = this.querySelector<HTMLButtonElement>("[data-open-search]");
		const dialog = this.querySelector("dialog");
		const input = this.querySelector("input");
		const status = this.querySelector<HTMLParagraphElement>(".status");
		const results = this.querySelector<HTMLUListElement>(".results");
		const indexScript = document.querySelector<HTMLScriptElement>(
			"[data-offline-index]"
		);
		if (!(opener && dialog && input && status && results && indexScript)) {
			throw new Error("Incomplete offline search markup");
		}
		const pages = window.fplinuxOfflineIndex.map((page) => ({
			...page,
			searchText: `${page.title} ${page.text}`.normalize("NFKC").toLowerCase(),
		}));
		const root = new URL("../", indexScript.src);
		const open = () => {
			dialog.showModal();
			input.focus();
		};
		opener.disabled = false;
		opener.addEventListener("click", open);
		dialog.addEventListener("click", (event) => {
			if (event.target !== dialog) {
				return;
			}
			const box = dialog.getBoundingClientRect();
			if (
				event.clientX < box.left ||
				event.clientX > box.right ||
				event.clientY < box.top ||
				event.clientY > box.bottom
			) {
				dialog.close();
			}
		});
		window.addEventListener("keydown", (event) => {
			if (
				!(event.ctrlKey || event.metaKey) ||
				event.key.toLowerCase() !== "k"
			) {
				return;
			}
			event.preventDefault();
			if (dialog.open) {
				dialog.close();
			} else {
				open();
			}
		});
		input.addEventListener("input", () => {
			const terms =
				input.value
					.normalize("NFKC")
					.toLowerCase()
					.match(/[\p{L}\p{N}_+-]+/gu) ?? [];
			results.replaceChildren();
			if (terms.length === 0) {
				status.textContent = "";
				return;
			}
			const matches = pages.filter((page) =>
				terms.every((term) => page.searchText.includes(term))
			);
			const score = (page: SearchPage) =>
				terms.filter((term) => page.title.toLowerCase().includes(term)).length;
			matches.sort((left, right) => score(right) - score(left));
			status.textContent = matches.length
				? `${matches.length} pages found; showing up to 20.`
				: "No results found.";
			for (const page of matches.slice(0, 20)) {
				const item = document.createElement("li");
				const link = document.createElement("a");
				link.href = new URL(page.url, root).href;
				link.textContent = page.title;
				const excerpt = document.createElement("p");
				const first = page.text.toLowerCase().indexOf(terms[0] ?? "");
				const start = Math.max(0, first - 60);
				excerpt.textContent = `${start ? "…" : ""}${page.text.slice(start, start + 220)}…`;
				item.append(link, excerpt);
				results.append(item);
			}
		});
	}
}
customElements.define("offline-site-search", OfflineSiteSearch);
