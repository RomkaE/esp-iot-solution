COMPONENT_ADD_INCLUDEDIRS := components/features/touchpad/include

COMPONENT_SRCDIRS := components/features/touchpad

ifdef CONFIG_DATA_SCOPE_DEBUG
COMPONENT_ADD_INCLUDEDIRS += components/features/touchpad/scope_debug	
COMPONENT_SRCDIRS += components/features/touchpad/scope_debug
endif