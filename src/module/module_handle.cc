#include "module_handle.h"
#include "context_handle.h"
#include "reference_handle.h"
#include "transferable.h"
#include "isolate/class_handle.h"
#include "isolate/run_with_timeout.h"
#include "isolate/three_phase_task.h"

#include <algorithm>

using namespace v8;
using std::shared_ptr;

namespace ivm {


namespace {

auto LookupModuleInfo(Local<Module> module) {
	auto& module_map = IsolateEnvironment::GetCurrent().module_handles;
	auto range = module_map.equal_range(module->GetIdentityHash());
	auto it = std::find_if(range.first, range.second, [&](decltype(*module_map.begin()) data) {
		return data.second->handle.Deref() == module;
	});
	return it == range.second ? nullptr : it->second;
}

} // anonymous namespace

ModuleInfo::ModuleInfo(Local<Module> handle) : identity_hash{handle->GetIdentityHash()}, handle{handle} {
	// Add to isolate's list of modules
	IsolateEnvironment::GetCurrent().module_handles.emplace(identity_hash, this);
	// Grab all dependency specifiers
	Isolate* isolate = Isolate::GetCurrent();
	auto context = isolate->GetCurrentContext();
	auto& requests = **handle->GetModuleRequests();
	dependency_specifiers.reserve(requests.Length());
	for (int ii = 0; ii < requests.Length(); ii++) {
		auto request = requests.Get(context, ii).As<ModuleRequest>();
		dependency_specifiers.emplace_back(*String::Utf8Value{isolate, request->GetSpecifier()});
	}
}

ModuleInfo::~ModuleInfo() {
	// Remove from isolate's list of modules
	auto environment = handle.GetIsolateHolder()->GetIsolate();
	if (environment) {
		auto& module_map = environment->module_handles;
		auto range = module_map.equal_range(identity_hash);
		auto it = std::find_if(range.first, range.second, [&](decltype(*module_map.begin()) data) {
			return this == data.second;
		});
		assert(it != range.second);
		module_map.erase(it);
	}
}

ModuleHandle::ModuleHandleTransferable::ModuleHandleTransferable(shared_ptr<ModuleInfo> info) : info(std::move(info)) {}

auto ModuleHandle::ModuleHandleTransferable::TransferIn() -> Local<Value> {
	return ClassHandle::NewInstance<ModuleHandle>(info);
};

ModuleHandle::ModuleHandle(shared_ptr<ModuleInfo> info) : info(std::move(info)) {}

auto ModuleHandle::Definition() -> Local<FunctionTemplate> {
	return Inherit<TransferableHandle>(MakeClass(
		"Module", nullptr,
		"dependencySpecifiers", MemberAccessor<decltype(&ModuleHandle::GetDependencySpecifiers), &ModuleHandle::GetDependencySpecifiers>{},
		"instantiate", MemberFunction<decltype(&ModuleHandle::Instantiate), &ModuleHandle::Instantiate>{},
		"instantiateSync", MemberFunction<decltype(&ModuleHandle::InstantiateSync), &ModuleHandle::InstantiateSync>{},
		"evaluate", MemberFunction<decltype(&ModuleHandle::Evaluate<1>), &ModuleHandle::Evaluate<1>>{},
		"evaluateSync", MemberFunction<decltype(&ModuleHandle::Evaluate<0>), &ModuleHandle::Evaluate<0>>{},
		"namespace", MemberAccessor<decltype(&ModuleHandle::GetNamespace), &ModuleHandle::GetNamespace>{},
		"release", MemberFunction<decltype(&ModuleHandle::Release), &ModuleHandle::Release>{}
	));
}

auto ModuleHandle::TransferOut() -> std::unique_ptr<Transferable> {
	return std::make_unique<ModuleHandleTransferable>(info);
}

auto ModuleHandle::GetDependencySpecifiers() -> Local<Value> {
	Isolate* isolate = Isolate::GetCurrent();
	size_t length = info->dependency_specifiers.size();
	Local<Array> deps = Array::New(isolate, length);
	for (size_t ii = 0; ii < length; ++ii) {
		Unmaybe(deps->Set(isolate->GetCurrentContext(), ii, v8_string(info->dependency_specifiers[ii].c_str())));
	}
	return deps;
}

auto ModuleHandle::GetInfo() const -> std::shared_ptr<ModuleInfo> {
	if (!info) {
		throw RuntimeGenericError("Module has been released");
	}
	return info;
}

auto ModuleHandle::Release() -> Local<Value> {
	info.reset();
	return Undefined(Isolate::GetCurrent());
}


static void ImportResolved( const FunctionCallbackInfo<Value> &info );
static void ImportRejected( const FunctionCallbackInfo<Value> &info );


struct pu8pair {
	v8::String::Utf8Value *key;
	v8::String::Utf8Value *value;
};

struct pendingImport {
	v8::Persistent<v8::Promise::Resolver> resolver;
	std::shared_ptr<IsolateHolder> holder;
	pendingImport(
	     v8::Isolate *target_isolate
		, v8::Persistent<v8::Promise::Resolver> &resolver
	             , std::shared_ptr<IsolateHolder> holder )
	    : resolver{target_isolate, resolver }
	    , holder( holder ) {
//		this->resolver.Reset( Isolate::GetCurrent(), resolver );
	 }
};

v8::Local<v8::Object> GetInternalClass( Isolate *isolate ) {
	// Prepare constructor template

	v8::MaybeLocal<v8::FunctionTemplate> classTemplate
	     = v8::FunctionTemplate::New( isolate );
	v8::Local<v8::FunctionTemplate> classTemplate2
	     = classTemplate.ToLocalChecked();
	classTemplate2->SetClassName(
	     String::NewFromUtf8Literal( isolate, "ivm::InternalTransport" ) );
	classTemplate2->InstanceTemplate()->SetInternalFieldCount(
	     1 ); // 1 required for arbitrary C++ pointer

	Local<Function> fn
	     = classTemplate2->GetFunction( isolate->GetCurrentContext() )
	          .ToLocalChecked();
	return fn->NewInstance( isolate->GetCurrentContext(), 0, nullptr ).ToLocalChecked();
}

class ImportModuleDynamicallyTask : public Runnable {

 public:
	ImportModuleDynamicallyTask( std::shared_ptr<IsolateHolder> holder
	                           , std::shared_ptr<IsolateHolder> importing_holder
	                           , Local<Promise::Resolver> resolver
	                           , Local<Data> host_defined_options
	                           , String::Utf8Value* resource_name
	                           , String::Utf8Value * specifier
	                           , int pairs
	                           , pu8pair * import_attributes )
	    : holder( holder )
	    , importing_holder( importing_holder )
	    , resolver{ resolver->GetIsolate(), resolver }
	    , host_defined_options{ Isolate::GetCurrent(), host_defined_options }
	    , resource_name( resource_name )
	    , specifier(specifier )
	    , pairs( pairs )
	    , import_attributes(  import_attributes ) {}
	void Run() final {
		Isolate *isolate = Isolate::GetCurrent();
		printf( "Got callback import?\n" );
		fflush( stdout );
		/*
		Locker locker( isolate );
		if( !locker.IsLocked( isolate ) ) {
		   printf( "Have to wait to lock? Can we ever lock?\n" );
		   fflush( stdout );
		}
		*/

		HandleScope handle_scope( isolate );
		Local<Context> context  = isolate->GetCurrentContext();
		// Context::Scope context_scope{
		//		     IsolateEnvironment::GetCurrent().DefaultContext() };
		IsolateEnvironment &env = IsolateEnvironment::GetCurrent();
		// shared_ptr<IsolateHolder> holder   = env.GetCurrentHolder();
		v8::Local<v8::Object> opts
		     = host_defined_options.Get( isolate ).As<Object>();

		v8::Local<v8::Object> attribs = Object::New( isolate );
		for( int i = 0; i < pairs; i++ ) {
			attribs->Set( context, v8_string( ( **import_attributes[ i ].key ) )
			            , v8_string( ( **import_attributes[ i ].value ) ) );
		}
		Local<Value> args[ 3 ]
		     = { v8_string( **specifier ), v8_string( **resource_name ), attribs };

		Local<Function> cb = holder->import_dynamic_callback.Get( isolate );
		Local<Context> ctx = this->context.Get( isolate );
		if( !cb.IsEmpty() ) {
			Local<Value> promise_result = cb
			     ->Call( isolate->GetCurrentContext(), Null( isolate ), 3, args )
			     .ToLocalChecked();
			Local<Promise> pr         = promise_result.As<Promise>();
			if( !pr.IsEmpty() ) {
				Local<Object> importExtra = GetInternalClass( isolate );
				pendingImport *pending_import
				     = new pendingImport( *holder->GetIsolate(), resolver, holder );
				// printf( "Resolver isolate still? %p\n", resolverGetIsolate() );
				importExtra->SetAlignedPointerInInternalField( 0, pending_import );

				Local<Function> ft
				     = Function::New( context, ImportResolved, importExtra )
				          .ToLocalChecked();
				Local<Function> fc
				     = Function::New( context, ImportRejected, importExtra )
				          .ToLocalChecked();
				MaybeLocal<Promise> result = pr->Then( context, ft, fc );
			}
		}
		// this->Run2( isolate, isolate->GetCurrentContext() );

		isolate->PerformMicrotaskCheckpoint();
	}

 public:
	Persistent<Context> context;
	v8::Persistent<v8::Promise::Resolver> resolver;
	Persistent<Data> host_defined_options;
	String::Utf8Value *resource_name;
	String::Utf8Value* specifier;
	int pairs;
	pu8pair *import_attributes;
	shared_ptr<IsolateHolder> holder;
	shared_ptr<IsolateHolder> importing_holder;
};



struct ImportModuleDynamicallyResolveTask : public Runnable {
	Persistent<Value> module;
	Isolate *module_isolate;
	//class ImportModuleDynamicallyTask *from; 
	Persistent<Promise::Resolver> promise;
	bool success;
	ImportModuleDynamicallyResolveTask( Isolate *module_isolate
	                                  , Persistent<Promise::Resolver> & promise
	                                  , Local<Value> module )
	    : module_isolate( module_isolate )
	    , promise{ Isolate::GetCurrent(), promise }
	    , module{ Isolate::GetCurrent() , module } {}
	void Run() final {
		// this is running in the VM isolate thread.
		// and the module we know is in the host isolate?
		Isolate *ivm_isolate = *Executor::GetCurrentEnvironment();
		Isolate *isolate = Isolate::GetCurrent();
		HandleScope handle_scope( isolate );
		Local<Context> context = Executor::GetCurrentEnvironment()->DefaultContext();
		Context::Scope ct( context );
		printf( "Module isn't in this isolate... %p %p", module_isolate
		      , isolate );

		Local<Object> module = this->module.Get( module_isolate ).As<Object>();
		Local<Context> ct2 = module->GetCreationContext().ToLocalChecked();
		Local<Value> name           = module->GetConstructorName();
		// printf( "Resolving Final Promise here----------------- %s\n", *String::Utf8Value( module_isolate, name ) );
		 Local<TransferableHandle> dr = module.As<TransferableHandle>();
		 std::unique_ptr<Transferable> drval = dr->TransferOut();

		Local<Promise::Resolver> pr = promise.Get( isolate );

		pr->Resolve( isolate->GetCurrentContext(), drval->TransferIn() );

		printf( "dispatch resolved promises??\n" );
		isolate->PerformMicrotaskCheckpoint();
		printf( "Finish run...\n" );
	}
};

struct ImportModuleDynamicallyRejectTask : public Runnable {
	class ImportModuleDynamicallyTask *from;
	bool success;
	ImportModuleDynamicallyRejectTask(
	     class ImportModuleDynamicallyTask *from ): from(from) {}
	void Run() final {
		
		Isolate *isolate = Isolate::GetCurrent();
		HandleScope handle_scope( isolate );
		from->resolver.Get( isolate )->Reject(
		     from->context.Get( isolate ), Undefined( Isolate::GetCurrent() ) );
	}
};

static void ImportResolved( const
                               FunctionCallbackInfo<Value> &info ) {
	// this is a callback triggered in the host isolate.
	// it comes from the Host code linker resolution.
	// info should be an ivm module... which is portable?
	Isolate *isolate                  = Isolate::GetCurrent();
	// this needs to be posted to the VM to resolve the import()
	// in the vm isolate.
	Local<Object> importExtra = info.Data().As<Object>();
	Local<Promise::Resolver> resolver = importExtra
	     ->Get( info.GetIsolate()->GetCurrentContext(), v8_string( "resolver" ) )
	     .ToLocalChecked()
	     .As<Promise::Resolver>();
	//Local<Object>  = info[0].As<Object>();

	// this is back in the UV context/thread
	pendingImport *pending_import
	     = (pendingImport *)importExtra->GetAlignedPointerFromInternalField( 0 );
	shared_ptr<IsolateHolder> importing_holder = pending_import->holder;

	     //= (IsolateHolder *)importExtra->GetAlignedPointerFromInternalField( 0 );

	//importing_holder = o.Get( info.GetIsolate() );
	Local<Object> module = info[ 0 ].As<Object>();
	Local<Module> m2                           = Local<Module>::Cast( module );
	//m2->
	String::Utf8Value name( info.GetIsolate(), module->GetConstructorName() );

	Local<ReferenceHandle> ns = module
	     ->Get( info.GetIsolate()->GetCurrentContext()
	          , v8_string( "namespace" ) )
	     .ToLocalChecked()
	     .As<ReferenceHandle>();
	String::Utf8Value nsName( ns.As<Object>()->GetIsolate()
	                        , ns.As<Object>()->GetConstructorName() );
	Local<Value> dri = ns->DerefInto( MaybeLocal<Object>() );
	//Isolate *chk = ns->GetIsolate();
	//Local<Context> chkctx = ns->GetCreationContext().ToLocalChecked();
	//Local<Array> names = module->GetPropertyNames( info.GetIsolate()->GetCurrentContext() ).ToLocalChecked();



	importing_holder->ScheduleTask(
	     std::make_unique<struct ImportModuleDynamicallyResolveTask>(
	          info.GetIsolate(), pending_import->resolver, dri )
			     , false, true, false );
	
}

void ImportRejected( const FunctionCallbackInfo<Value> &info ) {
	// this is a callback triggered in the host isolate.

	/*
			importing_holder->ScheduleTask(
			     std::make_unique<ImportModuleDynamicallyRejectTask>(this)
			     , false, true, false );
	*/
}


v8::MaybeLocal<v8::Promise>
ModuleHandle::ImportModuleDynamically( v8::Local<v8::Context> context
                                     , v8::Local<v8::Data> host_defined_options
                                     , v8::Local<v8::Value> resource_name
                                     , v8::Local<v8::String> specifier
                                     , v8::Local<v8::FixedArray> import_attributes ) {
	Isolate* isolate = Isolate::GetCurrent();
	String::Utf8Value *r_name = new String::Utf8Value ( isolate, resource_name );
	String::Utf8Value *spec   = new String::Utf8Value( isolate, specifier );

#if 0
	// never had host_defined_options with content; but the one I did get
	// was a FixedArray with 0 length.
	// v8::Local<v8::Value> hdo   = Local<Value>::Cast( host_defined_options );
	if( host_defined_options->IsValue() ) {
		//printf( "Host Defined Options is an Value\n" );
	} else if( host_defined_options->IsModule() ) {
		//printf( "Host Defined Options is a IsModule\n" );
	} else if( host_defined_options->IsContext() ) {
		//printf( "Host Defined Options is a IsContext\n" );
	} else if( host_defined_options->IsPrivate() ) {
		//printf( "Host Defined Options is a IsPrivate\n" );
	} else if( host_defined_options->IsFixedArray() ) {
		/*
		Local<FixedArray> fa = Local<FixedArray>::Cast( host_defined_options );
		for( int i = 0; i < fa->Length(); i++ ) {
			v8::Local<v8::Data> v   = fa->Get( context, i );
			v8::Local<v8::Value> v2 = Local<Value>::Cast( v );
			// printf( "host_defined_options[%d]: %s\n", i
			//			      , *String::Utf8Value( isolate, v2 ) );
		}
		*/
		// printf( "Host Defined Options is a IsFixedArray\n" );
	} else if( host_defined_options->IsFunctionTemplate() ) {
		// printf( "Host Defined Options is a IsFunctionTemplate\n" );
	} else {
		//printf( "Host Defined Options is a ???\n"  );
	}
#endif

	pu8pair *attribs = new pu8pair[ import_attributes->Length() ];
	for( int i = 0; i < import_attributes->Length(); i++ ) {
		v8::Local<v8::Data> v = import_attributes->Get( context, i );
		v8::Local<v8::Value> v2 = Local<Value>::Cast( v );
		if( i & 1 )
			attribs[ i/2 ].value = new String::Utf8Value( isolate, v2 );
		else
			attribs[ i/2 ].key   = new String::Utf8Value( isolate, v2 );
	}
	
	// this is called from the import() method
	// which will be running in the ivm instance; and context
	// so this has to be uv_scheduled to the main thread
	Local<Promise::Resolver> resolver = Unmaybe( Promise::Resolver::New( context ) );
	//IsolateEnvironment& defenv = Executor::GetDefaultEnvironment();
	printf( "Resolver isolate %p %p\n", isolate, resolver->GetIsolate() );
	shared_ptr<IsolateHolder> holder = IsolateEnvironment::GetCurrent().GetCurrentHolder();
	if( !holder->import_dynamic_callback.IsEmpty() ) {
		holder->import_dynamic_callback_host_isolate->ScheduleTask(
		     std::make_unique<ImportModuleDynamicallyTask>(
		          holder, holder->import_dynamic_callback_host_isolate, resolver
		          , host_defined_options, r_name, spec, import_attributes->Length()/2, attribs )
		     , false, true, false );
	}else
		resolver->Reject( context, v8_string( "dynamic import callback not registered" ) );

	return MaybeLocal<Promise>( resolver->GetPromise() );
}


void ModuleHandle::InitializeImportMeta(Local<Context> context, Local<Module> module, Local<Object> meta) {
	ModuleInfo* found = LookupModuleInfo(module);
	if (found != nullptr) {
		if (found->meta_callback) {
			detail::RunBarrier([&]() {
				Local<Value> argv[1];
				argv[0] = meta;
				Unmaybe(found->meta_callback.Deref()->Call(context, Undefined(context->GetIsolate()), 1, argv));
			});
		}
	}
}

/**
 * Implements the module linking logic used by `instantiate`. This is implemented as a class handle
 * so v8 can manage the lifetime of the linker. If a promise fails to resolve then v8 will be
 * responsible for calling the destructor.
 */
class ModuleLinker : public ClassHandle {
	public:
		/**
		 * These methods are split out from the main class so I don't have to recreate the class
		 * inheritance in v8
		 */
		struct Implementation {
			RemoteHandle<Object> linker;
			explicit Implementation(Local<Object> linker) : linker(linker) {}
			virtual ~Implementation() = default;
			virtual void HandleCallbackReturn(ModuleHandle* module, size_t ii, Local<Value> value) = 0;
			virtual auto Begin(ModuleHandle& module, RemoteHandle<Context> context) -> Local<Value> = 0;
			auto GetLinker() const -> ModuleLinker& {
				auto* ptr = ClassHandle::Unwrap<ModuleLinker>(linker.Deref());
				assert(ptr);
				return *ptr;
			}
		};

	private:
		RemoteHandle<Function> callback;
		std::unique_ptr<Implementation> impl;
		std::vector<std::shared_ptr<ModuleInfo>> modules;

	public:
		static auto Definition() -> v8::Local<v8::FunctionTemplate> {
			return MakeClass("Linker", nullptr);
		}

		explicit ModuleLinker(Local<Function> callback) : callback(callback) {}

		ModuleLinker(const ModuleLinker&) = delete;
		auto operator=(const ModuleLinker&) = delete;

		~ModuleLinker() override {
			Reset();
		}

		template <typename T>
		void SetImplementation() {
			impl = std::make_unique<T>(This());
		}

		template <typename T>
		auto GetImplementation() -> T* {
			return dynamic_cast<T*>(impl.get());
		}

		auto Begin(ModuleHandle& module, RemoteHandle<Context> context) -> Local<Value> {
			return impl->Begin(module, std::move(context));
		}

		void ResolveDependency(size_t ii, ModuleInfo& module, ModuleHandle* dependency) {
			{
				// I don't think the lock is actually needed here because this linker has already claimed
				// the whole module, and this code will only be running in a single thread.. but putting
				// up the lock is probably good practice or something.
				std::lock_guard<std::mutex> lock(module.mutex);
				module.resolutions[module.dependency_specifiers[ii]] = dependency->GetInfo();
			}
			Link(dependency);
		}

		void Link(ModuleHandle* module) {
			// Check current link status
			auto info = module->GetInfo();
			{
				std::lock_guard<std::mutex> lock(info->mutex);
				switch (info->link_status) {
					case ModuleInfo::LinkStatus::None:
						info->link_status = ModuleInfo::LinkStatus::Linking;
						info->linker = this;
						break;
					case ModuleInfo::LinkStatus::Linking:
						if (info->linker != this) {
							printf( "ivm:module_handle:linker and this: %p %p\n", info->linker, this );
							fflush( stdout );
							//throw RuntimeGenericError("Module is currently being linked by another linker");
						}
						return;
					case ModuleInfo::LinkStatus::Linked:
						return;
				}
			}
			// Recursively link
			modules.emplace_back(info);
			Isolate* isolate = Isolate::GetCurrent();
			Local<Context> context = isolate->GetCurrentContext();
			Local<Value> recv = Undefined(isolate);
			Local<Value> argv[2];
			argv[1] = module->This();
			Local<Function> fn = callback.Deref();
			for (size_t ii = 0; ii < info->dependency_specifiers.size(); ++ii) {
				argv[0] = v8_string(info->dependency_specifiers[ii].c_str());
				impl->HandleCallbackReturn(module, ii, Unmaybe(fn->Call(context, recv, 2, argv)));
			}
		}

		void Reset(ModuleInfo::LinkStatus status = ModuleInfo::LinkStatus::None) {
			// Clears out dependency info. If the module wasn't instantiated this resets them back to
			// their original state. If it was instantiated then we don't need the dependencies anymore
			// anyway.
			for (auto& module : modules) {
				std::lock_guard<std::mutex> lock(module->mutex);
				module->linker = nullptr;
				module->link_status = status;
				module->resolutions.clear();
			}
			modules.clear();
			impl.reset();
		}
};

/**
 * Runner for `instantiate`. By the time this is invoked the module will already have all its
 * dependencies resolved by the linker.
 */
struct InstantiateRunner : public ThreePhaseTask {
	RemoteHandle<Context> context;
	shared_ptr<ModuleInfo> info;
	RemoteHandle<Object> linker;

	static auto ResolveCallback(Local<Context> /*context*/, Local<String> specifier, Local<FixedArray> /*import_assertions*/, Local<Module> referrer) -> MaybeLocal<Module> {
		MaybeLocal<Module> ret;
		detail::RunBarrier([&]() {
			// Lookup ModuleInfo* instance from `referrer`
			ModuleInfo* found = LookupModuleInfo(referrer);
			if (found != nullptr) {
				// nb: lock is already acquired in `Instantiate`
				auto& resolutions = found->resolutions;
				auto it = resolutions.find(*String::Utf8Value{Isolate::GetCurrent(), specifier});
				if (it != resolutions.end()) {
					ret = it->second->handle.Deref();
					return;
				}
			}
			throw RuntimeGenericError("Dependency was left unresolved. Please report this error on github.");
		});
		return ret;
	}

	InstantiateRunner(
		RemoteHandle<Context> context,
		shared_ptr<ModuleInfo> info,
		Local<Object> linker
	) :
		context(std::move(context)),
		info(std::move(info)),
		linker(linker) {
		// Sanity check
		if (this->info->handle.GetIsolateHolder() != this->context.GetIsolateHolder()) {
			throw RuntimeGenericError("Invalid context");
		}
	}

	void Phase2() final {
		Local<Module> mod = info->handle.Deref();
		Local<Context> context_local = context.Deref();
		info->context_handle = std::move(context);
		std::lock_guard<std::mutex> lock{info->mutex};
		TryCatch try_catch{Isolate::GetCurrent()};
		try {
			Unmaybe(mod->InstantiateModule(context_local, ResolveCallback));
		} catch (...) {
			try_catch.ReThrow();
			throw;
		}
		// `InstantiateModule` will return Maybe<bool>{true} even when there are exceptions pending.
		// This condition is checked here and a C++ is thrown which will propagate out as a JS
		// exception.
		if (try_catch.HasCaught()) {
			try_catch.ReThrow();
			throw RuntimeError();
		}
	}

	auto Phase3() -> Local<Value> final {
		ClassHandle::Unwrap<ModuleLinker>(linker.Deref())->Reset(ModuleInfo::LinkStatus::Linked);
		return Undefined(Isolate::GetCurrent());
	}
};

/**
 * Async / sync implementations of the linker
 */
class ModuleLinkerSync : public ModuleLinker::Implementation {
	private:
		void HandleCallbackReturn(ModuleHandle* module, size_t ii, Local<Value> value) final {
			ModuleHandle* resolved = value->IsObject() ? ClassHandle::Unwrap<ModuleHandle>(value.As<Object>()) : nullptr;
			if (resolved == nullptr) {
				throw RuntimeTypeError("Resolved dependency was not `Module`");
			}
			GetLinker().ResolveDependency(ii, *module->GetInfo(), resolved);
		}

	public:
		using ModuleLinker::Implementation::Implementation;
		auto Begin(ModuleHandle& module, RemoteHandle<Context> context) -> Local<Value> final {
			try {
				GetLinker().Link(&module);
			} catch (const RuntimeError& err) {
				GetLinker().Reset();
				throw;
			}
			auto info = module.GetInfo();
			return ThreePhaseTask::Run<0, InstantiateRunner>(*info->handle.GetIsolateHolder(), context, info, linker.Deref());
		}
};

class ModuleLinkerAsync : public ModuleLinker::Implementation {
	private:
		RemoteTuple<Promise::Resolver, Function> async_handles;
		RemoteHandle<Context> context;
		shared_ptr<ModuleInfo> info;
		uint32_t pending = 0;

		static auto ModuleResolved(Local<Array> holder, Local<Value> value) -> Local<Value> {
			detail::RunBarrier([&]() {
				ModuleHandle* resolved = value->IsObject() ? ClassHandle::Unwrap<ModuleHandle>(value.As<Object>()) : nullptr;
				if (resolved == nullptr) {
					throw RuntimeTypeError("Resolved dependency was not `Module`");
				}
				Local<Context> context = Isolate::GetCurrent()->GetCurrentContext();
				auto* linker = ClassHandle::Unwrap<ModuleLinker>(Unmaybe(holder->Get(context, 0)).As<Object>());
				auto* impl = linker->GetImplementation<ModuleLinkerAsync>();
				if (impl == nullptr) {
					return;
				}
				auto* module = ClassHandle::Unwrap<ModuleHandle>(Unmaybe(holder->Get(context, 1)).As<Object>());
				auto ii = Unmaybe(holder->Get(context, 2)).As<Uint32>()->Value();
				linker->ResolveDependency(ii, *module->GetInfo(), resolved);
				if (--impl->pending == 0) {
					impl->Instantiate();
				}
			});
			return Undefined(Isolate::GetCurrent());
		}

		static auto ModuleRejected(ModuleLinker& linker, Local<Value> error) -> Local<Value> {
			detail::RunBarrier([&]() {
				auto* impl = linker.GetImplementation<ModuleLinkerAsync>();
				if (impl != nullptr) {
					Unmaybe(impl->async_handles.Deref<0>()->Reject(Isolate::GetCurrent()->GetCurrentContext(), error));
					linker.Reset();
				}
			});
			return Undefined(Isolate::GetCurrent());
		}

		void HandleCallbackReturn(ModuleHandle* module, size_t ii, Local<Value> value) final {
			// Resolve via Promise.resolve() so thenables will work
			++pending;
			Isolate* isolate = Isolate::GetCurrent();
			Local<Context> context = isolate->GetCurrentContext();
			Local<Promise::Resolver> resolver = Unmaybe(Promise::Resolver::New(context));
			Local<Promise> promise = resolver->GetPromise();
			Local<Array> holder = Array::New(isolate, 3);
			Unmaybe(holder->Set(context, 0, linker.Deref()));
			Unmaybe(holder->Set(context, 1, module->This()));
			Unmaybe(holder->Set(context, 2, Uint32::New(isolate, ii)));
			promise = Unmaybe(promise->Then(context, Unmaybe(
				Function::New(context, FreeFunctionWithData<decltype(&ModuleResolved), &ModuleResolved>{}.callback, holder)
			)));
			Unmaybe(promise->Catch(context, async_handles.Deref<1>()));
			Unmaybe(resolver->Resolve(context, value));
		}

		void Instantiate() {
			Unmaybe(async_handles.Deref<0>()->Resolve(
				Isolate::GetCurrent()->GetCurrentContext(),
				ThreePhaseTask::Run<1, InstantiateRunner>(*info->handle.GetIsolateHolder(), context, info, linker.Deref())
			));
		}

	public:
		explicit ModuleLinkerAsync(Local<Object> linker) : Implementation(linker), async_handles(
			Unmaybe(Promise::Resolver::New(Isolate::GetCurrent()->GetCurrentContext())),
			Unmaybe(Function::New(
				Isolate::GetCurrent()->GetCurrentContext(),
				FreeFunctionWithData<decltype(&ModuleRejected), &ModuleRejected>{}.callback, linker)
			)
		 ) {}

		using ModuleLinker::Implementation::Implementation;
		auto Begin(ModuleHandle& module, RemoteHandle<Context> context) -> Local<Value> final {
			GetLinker().Link(&module);
			info = module.GetInfo();
			this->context = std::move(context);
			if (pending == 0) {
				Instantiate();
			}
			return async_handles.Deref<0>()->GetPromise();
		}
};

auto ModuleHandle::Instantiate(ContextHandle& context_handle, Local<Function> callback) -> Local<Value> {
	auto context = context_handle.GetContext();
	Local<Object> linker_handle = ClassHandle::NewInstance<ModuleLinker>(callback);
	auto* linker = ClassHandle::Unwrap<ModuleLinker>(linker_handle);
	linker->SetImplementation<ModuleLinkerAsync>();
	return linker->Begin(*this, context);
}

auto ModuleHandle::InstantiateSync(ContextHandle& context_handle, Local<Function> callback) -> Local<Value> {
	auto context = context_handle.GetContext();
	Local<Object> linker_handle = ClassHandle::NewInstance<ModuleLinker>(callback);
	auto* linker = ClassHandle::Unwrap<ModuleLinker>(linker_handle);
	linker->SetImplementation<ModuleLinkerSync>();
	return linker->Begin(*this, context);
}

struct EvaluateRunner : public ThreePhaseTask {
	shared_ptr<ModuleInfo> info;
	std::unique_ptr<Transferable> result;
	uint32_t timeout;

	EvaluateRunner(shared_ptr<ModuleInfo> info, uint32_t ms) : info(std::move(info)), timeout(ms) {}

	void Phase2() final {
		Local<Module> mod = info->handle.Deref();
		if (mod->GetStatus() == Module::Status::kUninstantiated) {
			throw RuntimeGenericError("Module is uninstantiated");
		}
		Local<Context> context_local = Deref(info->context_handle);
		Context::Scope context_scope(context_local);
		result = OptionalTransferOut(RunWithTimeout(timeout, [&]() { return mod->Evaluate(context_local); }));
		std::lock_guard<std::mutex> lock(info->mutex);
		info->global_namespace = RemoteHandle<Value>(mod->GetModuleNamespace());
	}

	auto Phase3() -> Local<Value> final {
		if (result) {
			return result->TransferIn();
		} else {
			return Undefined(Isolate::GetCurrent()).As<Value>();
		}
	}
};

template <int async>
auto ModuleHandle::Evaluate(MaybeLocal<Object> maybe_options) -> Local<Value> {
	auto info = GetInfo();
	int32_t timeout_ms = ReadOption<int32_t>(maybe_options, StringTable::Get().timeout, 0);
	return ThreePhaseTask::Run<async, EvaluateRunner>(*info->handle.GetIsolateHolder(), info, timeout_ms);
}

auto ModuleHandle::GetNamespace() -> Local<Value> {
	std::lock_guard<std::mutex> lock(info->mutex);
	if (!info->global_namespace) {
		throw RuntimeGenericError("Module has not been instantiated.");
	}
	return ClassHandle::NewInstance<ReferenceHandle>(info->handle.GetSharedIsolateHolder(), info->global_namespace, info->context_handle, ReferenceHandle::TypeOf::Object, true, false);
}

} // namespace ivm
