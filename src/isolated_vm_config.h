#ifdef _WIN32
#	ifdef ISOLATED_VM_INTERNAL
#		define ISOLATED_VM_EXPORT __declspec(dllexport)
#	else
#		define ISOLATED_VM_EXPORT __declspec(dllimport)
#	endif
#else
#define ISOLATED_VM_EXPORT 
#endif
