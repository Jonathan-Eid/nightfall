//! Presentation policy for missing Windows PCVR runtimes.

pub fn needs_steamvr_only(requires_steamvr: bool, steamvr: bool, monado: bool, vdxr: bool) -> bool {
    requires_steamvr && !steamvr && !monado && !vdxr
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registered_vdxr_keeps_runtime_choices_visible_without_steamvr() {
        assert!(!needs_steamvr_only(true, false, false, true));
    }

    #[test]
    fn missing_steamvr_without_alternatives_shows_install_message() {
        assert!(needs_steamvr_only(true, false, false, false));
    }

    #[test]
    fn other_available_runtimes_keep_choices_visible() {
        assert!(!needs_steamvr_only(true, false, true, false));
        assert!(!needs_steamvr_only(true, true, false, false));
        assert!(!needs_steamvr_only(false, false, false, false));
    }
}
