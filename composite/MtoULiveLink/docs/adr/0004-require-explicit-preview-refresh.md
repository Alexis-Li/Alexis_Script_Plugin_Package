# Require explicit preview refresh before connecting

Preview mode generates a Preview revision only when the user invokes Refresh
Preview in Unreal. Placement, level loading, reimport, and Maya connection may
mark or report the preview as not ready but never start generation implicitly.
Generation uses editor-only mesh APIs on Unreal's Game Thread and may take
seconds, while the existing Maya connection waits only five seconds for its
negotiation reply; explicit refresh prevents unexpected editor stalls and
misleading connection timeouts. Connect accepts only a ready revision, and the
existing protocol is not expanded with a building state for V1.
