#pragma once
// <colorpicker value="#RRGGBBAA" optional/>: a swatch that expands into an HSV square,
// hue bar and alpha bar. Works with data-value like a form control (dispatches "change"
// with a "value" parameter). `optional`: value 0 means "unset", shown as a slashed swatch.
namespace yap::colorpicker {

// Registers the element with RmlUi's factory. Call once after Rml::Initialise().
void register_element();

}  // namespace yap::colorpicker
