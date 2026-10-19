//! Internal page model and conversion to the C ABI structs.

use crate::abi::*;
use crate::util::copy_str;

#[derive(Debug, Clone, Default)]
pub struct Item {
    pub kind: i32,
    pub uri: String,
    pub title: String,
    pub subtitle: String,
    pub image_url: String,
    pub album_uri: String,
    pub artist_uri: String,
    pub duration_ms: u32,
    pub is_explicit: bool,
    pub is_playable: bool,
    pub is_saved: bool,
}

impl Item {
    pub fn to_c(&self) -> vam_item {
        let mut it = vam_item::default();
        it.kind = self.kind;
        copy_str(&mut it.uri, &self.uri);
        copy_str(&mut it.title, &self.title);
        copy_str(&mut it.subtitle, &self.subtitle);
        copy_str(&mut it.image_url, &self.image_url);
        copy_str(&mut it.album_uri, &self.album_uri);
        copy_str(&mut it.artist_uri, &self.artist_uri);
        it.duration_ms = self.duration_ms;
        it.is_explicit = self.is_explicit as u8;
        it.is_playable = self.is_playable as u8;
        it.is_saved = self.is_saved as u8;
        it
    }
}

#[derive(Debug, Clone, Default)]
pub struct Section {
    pub title: String,
    pub layout: i32,
    pub offset: u32,
    pub total: u32,
    pub items: Vec<Item>,
}

impl Section {
    pub fn list(title: impl Into<String>) -> Self {
        Section {
            title: title.into(),
            layout: VAM_LAYOUT_LIST,
            total: 0,
            offset: 0,
            items: Vec::new(),
        }
    }

    pub fn shelf(title: impl Into<String>) -> Self {
        Section {
            title: title.into(),
            layout: VAM_LAYOUT_SHELF,
            total: 0,
            offset: 0,
            items: Vec::new(),
        }
    }

    pub fn grid(title: impl Into<String>) -> Self {
        Section {
            title: title.into(),
            layout: VAM_LAYOUT_GRID,
            total: 0,
            offset: 0,
            items: Vec::new(),
        }
    }

    pub fn push(&mut self, item: Item) {
        self.items.push(item);
    }

    pub fn finish(&mut self) {
        self.total = self.items.len() as u32;
    }
}

#[derive(Debug, Clone, Default)]
pub struct Page {
    pub kind: i32,
    pub uri: String,
    pub title: String,
    pub subtitle: String,
    pub description: String,
    pub image_url: String,
    pub is_saved: bool,
    pub sections: Vec<Section>,
}

impl Page {
    pub fn new(kind: i32, uri: impl Into<String>) -> Self {
        Page {
            kind,
            uri: uri.into(),
            ..Default::default()
        }
    }

    /// Turn the page into a `*mut vam_page` owned by the backend.
    ///
    /// The C side only ever releases it through `vam_event_release`, which
    /// reconstructs the exact `Vec`s allocated here.
    pub fn into_raw(self) -> *mut vam_page {
        let sections: Vec<vam_section> = self
            .sections
            .into_iter()
            .map(|s| {
                let items = s.items.into_iter().map(|i| i.to_c()).collect::<Vec<_>>();
                let count = items.len() as u32;
                let items = if count == 0 {
                    std::ptr::null_mut()
                } else {
                    let mut boxed = items.into_boxed_slice();
                    let ptr = boxed.as_mut_ptr();
                    std::mem::forget(boxed);
                    ptr
                };
                vam_section {
                    title: {
                        let mut a = [0u8; VAM_TEXT_MAX];
                        copy_str(&mut a, &s.title);
                        a
                    },
                    layout: s.layout,
                    offset: s.offset,
                    total: s.total.max(count),
                    count,
                    items,
                }
            })
            .collect::<Vec<_>>();

        let section_count = sections.len() as u32;
        let sections_ptr = if section_count == 0 {
            std::ptr::null_mut()
        } else {
            let mut boxed = sections.into_boxed_slice();
            let ptr = boxed.as_mut_ptr();
            std::mem::forget(boxed);
            ptr
        };

        let mut page = Box::new(vam_page {
            kind: self.kind,
            uri: [0; VAM_URI_MAX],
            title: [0; VAM_TEXT_MAX],
            subtitle: [0; VAM_TEXT_MAX],
            description: [0; VAM_DESC_MAX],
            image_url: [0; VAM_URL_MAX],
            is_saved: self.is_saved as u8,
            reserved: [0; 3],
            section_count,
            sections: sections_ptr,
        });
        copy_str(&mut page.uri, &self.uri);
        copy_str(&mut page.title, &self.title);
        copy_str(&mut page.subtitle, &self.subtitle);
        copy_str(&mut page.description, &self.description);
        copy_str(&mut page.image_url, &self.image_url);
        Box::into_raw(page)
    }
}

/// Free a page previously produced by [`Page::into_raw`].
///
/// # Safety
/// `page` must be null or a pointer returned by `Page::into_raw` that has not
/// been freed yet.
pub unsafe fn free_page(page: *mut vam_page) {
    if page.is_null() {
        return;
    }
    let page = Box::from_raw(page);
    if !page.sections.is_null() && page.section_count > 0 {
        let sections = Vec::from_raw_parts(
            page.sections,
            page.section_count as usize,
            page.section_count as usize,
        );
        for section in &sections {
            if !section.items.is_null() && section.count > 0 {
                drop(Vec::from_raw_parts(
                    section.items,
                    section.count as usize,
                    section.count as usize,
                ));
            }
        }
        drop(sections);
    }
}

/// Free image bytes handed out in a `VAM_EV_IMAGE` event.
///
/// # Safety
/// `data`/`size` must come from a `Vec<u8>::into_raw_parts`-style allocation
/// made by this backend.
pub unsafe fn free_image(data: *const u8, size: u32) {
    if data.is_null() || size == 0 {
        return;
    }
    drop(Vec::from_raw_parts(data as *mut u8, size as usize, size as usize));
}
