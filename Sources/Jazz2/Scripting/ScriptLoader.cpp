﻿#if defined(WITH_ANGELSCRIPT)

#include "ScriptLoader.h"
#include "../ContentResolver.h"

#include <Containers/GrowableArray.h>
#include <Containers/StringConcatenable.h>
#include <IO/FileSystem.h>

#if defined(DEATH_TARGET_WINDOWS) && !defined(CMAKE_BUILD)
#   if defined(_M_X64)
#		if defined(_DEBUG)
#			pragma comment(lib, "../Libs/Windows/x64/angelscriptd.lib")
#		else
#			pragma comment(lib, "../Libs/Windows/x64/angelscript.lib")
#		endif
#   elif defined(_M_IX86)
#		if defined(_DEBUG)
#			pragma comment(lib, "../Libs/Windows/x86/angelscriptd.lib")
#		else
#			pragma comment(lib, "../Libs/Windows/x86/angelscript.lib")
#		endif
#   else
#       error Unsupported architecture
#   endif
#endif

using namespace Death::IO;

namespace Jazz2::Scripting
{
	ScriptLoader::ScriptLoader()
		: _module(nullptr), _scriptContextType(ScriptContextType::Unknown)
	{
		_engine = asCreateScriptEngine();
		_engine->SetEngineProperty(asEP_COPY_SCRIPT_SECTIONS, true);
		_engine->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, 2); // Required to allow chained assignment to properties
#if ANGELSCRIPT_VERSION >= 23600
		_engine->SetEngineProperty(asEP_IGNORE_DUPLICATE_SHARED_INTF, true);
#endif
		_engine->SetEngineProperty(asEP_COMPILER_WARNINGS, true);
#if !defined(DEATH_DEBUG)
		_engine->SetEngineProperty(asEP_BUILD_WITHOUT_LINE_CUES, true);
#endif
		_engine->SetUserData(this, EngineToOwner);
		_engine->SetContextCallbacks(RequestContextCallback, ReturnContextCallback, this);

		std::int32_t r = _engine->SetMessageCallback(asMETHOD(ScriptLoader, Message), this, asCALL_THISCALL); RETURN_ASSERT(r >= 0);

		_module = _engine->GetModule("Main", asGM_ALWAYS_CREATE); RETURN_ASSERT(_module != nullptr);
	}

	ScriptLoader::~ScriptLoader()
	{
		for (auto ctx : _contextPool) {
			ctx->Release();
		}

		if (_engine != nullptr) {
			_engine->ShutDownAndRelease();
			_engine = nullptr;
		}
	}

	ScriptContextType ScriptLoader::AddScriptFromFile(StringView path, const HashMap<String, bool>& definedSymbols)
	{
		String absolutePath = fs::GetAbsolutePath(path);
		if (absolutePath.empty()) {
			return ScriptContextType::Unknown;
		}

		// Include each file only once
		auto it = _includedFiles.find(absolutePath);
		if (it != _includedFiles.end()) {
			return ScriptContextType::AlreadyIncluded;
		}
		_includedFiles.emplace(absolutePath, true);

		auto s = fs::Open(absolutePath, FileAccess::Read);
		if (s->GetSize() <= 0) {
			return ScriptContextType::Unknown;
		}

		String scriptContent(NoInit, s->GetSize());
		s->Read(scriptContent.data(), s->GetSize());
		s->Dispose();

		ScriptContextType contextType = ScriptContextType::Legacy;
		SmallVector<String, 4> metadata;
		SmallVector<String, 0> includes;
		String currentClass, currentNamespace, metadataName, metadataDeclaration;
		std::int32_t scriptSize = (std::int32_t)scriptContent.size();

		// First perform the checks for #if directives to exclude code that shouldn't be compiled
		std::int32_t pos = 0;
		std::int32_t nested = 0;
		while (pos < scriptSize) {
			std::uint32_t len = 0;
			asETokenClass t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			if (t == asTC_UNKNOWN && scriptContent[pos] == '#' && (pos + 1 < scriptSize)) {
				std::int32_t start = pos++;

				t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);

				StringView token = scriptContent.slice(pos, pos + len);
				pos += len;

				if (token == "if"_s) {
					t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
					if (t == asTC_WHITESPACE) {
						pos += len;
						t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
					}

					if (t == asTC_IDENTIFIER) {
						StringView word = scriptContent.slice(pos, pos + len);
						pos += len;

						auto it = definedSymbols.find(String::nullTerminatedView(word));
						bool defined = (it != definedSymbols.end() && it->second);

						for (std::int32_t i = start; i < pos; i++) {
							if (scriptContent[i] != '\n') {
								scriptContent[i] = ' ';
							}
						}

						if (defined) {
							nested++;
						} else {
							pos = ExcludeCode(scriptContent, pos);
						}
					}
				} else if (token == "endif"_s) {
					// Only remove the #endif if there was a matching #if
					if (nested > 0) {
						for (std::int32_t i = start; i < pos; i++) {
							if (scriptContent[i] != '\n') {
								scriptContent[i] = ' ';
							}
						}
						nested--;
					}
				}
			} else
				pos += len;
		}

		pos = 0;
		while (pos < scriptSize) {
			std::uint32_t len = 0;
			asETokenClass t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			if (t == asTC_COMMENT || t == asTC_WHITESPACE) {
				pos += len;
				continue;
			}

			StringView token = scriptContent.slice(pos, pos + len);

			// Skip possible decorators before class and interface declarations
			if (token == "shared"_s || token == "abstract"_s || token == "mixin"_s || token == "external"_s) {
				pos += len;
				continue;
			}

			// Check if class or interface so the metadata for members can be gathered
			if (currentClass.empty() && (token == "class"_s || token == "interface"_s)) {
				do {
					pos += len;
					if (pos >= scriptSize) {
						t = asTC_UNKNOWN;
						break;
					}
					t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
				} while (t == asTC_COMMENT || t == asTC_WHITESPACE);

				if (t == asTC_IDENTIFIER) {
					currentClass = scriptContent.slice(pos, pos + len);

					while (pos < scriptSize) {
						_engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);

						if (scriptContent[pos] == '{') {
							pos += len;
							break;
						} else if (scriptContent[pos] == ';') {
							currentClass = "";
							pos += len;
							break;
						}

						pos += len;
					}
				}

				continue;
			}

			// Check if end of class
			if (!currentClass.empty() && token == "}"_s) {
				currentClass = { };
				pos += len;
				continue;
			}

			// Check if namespace so the metadata for members can be gathered
			if (token == "namespace"_s) {
				do {
					pos += len;
					t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
				} while (t == asTC_COMMENT || t == asTC_WHITESPACE);

				if (!currentNamespace.empty()) {
					currentNamespace += "::"_s;
				}
				currentNamespace += scriptContent.slice(pos, pos + len);

				// Search until first { is encountered
				while (pos < scriptSize) {
					_engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);

					// If start of namespace section encountered stop
					if (scriptContent[pos] == '{') {
						pos += len;
						break;
					}

					pos += len;
				}

				continue;
			}

			// Check if end of namespace
			if (!currentNamespace.empty() && token == "}"_s) {
				StringView found = currentNamespace.findLast("::"_s);
				if (found != nullptr) {
					currentNamespace = currentNamespace.prefix(found.begin());
				} else {
					currentNamespace = { };
				}
				pos += len;
				continue;
			}

			if (token == "["_s) {
				pos = ExtractMetadata(scriptContent, pos, metadata);

				MetadataType type;
				ExtractDeclaration(scriptContent, pos, metadataName, metadataDeclaration, type);

				if (type != MetadataType::Unknown) {
					_foundDeclarations.emplace_back(std::move(metadata), metadataName,
						metadataDeclaration, type, currentClass, currentNamespace);
				}
			} else if (token == "#"_s && (pos + 1 < scriptSize)) {
				std::int32_t start = pos++;

				t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
				if (t == asTC_IDENTIFIER) {
					token = scriptContent.slice(pos, pos + len);
					if (token == "include"_s) {
						pos += len;
						t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
						if (t == asTC_WHITESPACE) {
							pos += len;
							t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
						}

						if (t == asTC_VALUE && len > 2 && (scriptContent[pos] == '"' || scriptContent[pos] == '\'')) {
							StringView filename = StringView(&scriptContent[pos + 1], len - 2);
							StringView invalidChar = filename.findAny("\n\r\t");
							if (invalidChar != nullptr) {
								String str = "Invalid file name for #include - it contains a line-break or tab: \""_s + filename.prefix(invalidChar.begin()) + "\""_s;
								_engine->WriteMessage(path.data(), 0, 0, asMSGTYPE_ERROR, str.data());
							} else {
								String filenameProcessed = OnProcessInclude(filename, absolutePath);
								if (!filenameProcessed.empty()) {
									includes.push_back(filenameProcessed);
								}
							}
							pos += len;

							for (std::int32_t i = start; i < pos; i++) {
								if (scriptContent[i] != '\n') {
									scriptContent[i] = ' ';
								}
							}
						}
					} else if (token == "pragma"_s) {
						pos += len;
						for (; pos < scriptSize && scriptContent[pos] != '\n'; pos++);

						OnProcessPragma(scriptContent.slice(start + 7, pos).trimmed(), contextType);

						for (std::int32_t i = start; i < pos; i++) {
							if (scriptContent[i] != '\n') {
								scriptContent[i] = ' ';
							}
						}
					}
				} else {
					// Check for lines starting with #!, e.g. shebang interpreter directive. These will be treated as comments and removed by the preprocessor
					if (scriptContent[pos] == '!') {
						pos += len;
						for (; pos < scriptSize && scriptContent[pos] != '\n'; pos++);

						for (std::int32_t i = start; i < pos; i++) {
							if (scriptContent[i] != '\n') {
								scriptContent[i] = ' ';
							}
						}
					}
				}
			} else {
				// Don't search for metadata/includes within statement blocks or between tokens in statements
				pos = SkipStatement(scriptContent, pos);
			}
		}

		// Append the actual script
		_module->AddScriptSection(path.data(), scriptContent.data(), scriptSize, 0);

		if (includes.size() > 0) {
			// Load all included scripts
			for (auto& include : includes) {
				if (AddScriptFromFile(include, definedSymbols) == ScriptContextType::Unknown) {
					return ScriptContextType::Unknown;
				}
			}
		}

		return contextType;
	}

	ScriptBuildResult ScriptLoader::Build()
	{
		std::int32_t r = _module->Build();
		if (r < 0) {
			return (ScriptBuildResult)r;
		}

		// After the script has been built, the metadata strings should be stored for later lookup
		for (auto& decl : _foundDeclarations) {
			_module->SetDefaultNamespace(decl.Namespace.data());
			switch (decl.Type) {
				case MetadataType::Type: {
					std::int32_t typeId = _module->GetTypeIdByDecl(decl.Declaration.data());
					if (typeId >= 0) {
						auto entry = _typeMetadataMap.emplace(typeId, Array<String>(NoInit, decl.Metadata.size())).first;
						std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
					}
					break;
				}
				case MetadataType::Function: {
					if (decl.ParentClass.empty()) {
						asIScriptFunction* func = _module->GetFunctionByDecl(decl.Declaration.data());
						if (func != nullptr) {
							auto entry = _funcMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
					} else {
						std::int32_t typeId = _module->GetTypeIdByDecl(decl.ParentClass.data());
						asITypeInfo* type = _engine->GetTypeInfoById(typeId);
						asIScriptFunction* func = type->GetMethodByDecl(decl.Declaration.data());
						if (func != nullptr) {
							auto it = _classMetadataMap.find(typeId);
							if (it == _classMetadataMap.end()) {
								it = _classMetadataMap.emplace(typeId, ClassMetadata()).first;
							}

							auto entry = it->second.FuncMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
					}
					break;
				}
				case MetadataType::VirtualProperty: {
					if (decl.ParentClass.empty()) {
						asIScriptFunction* func = _module->GetFunctionByName(String("get_"_s + decl.Declaration).data());
						if (func != nullptr) {
							auto entry = _funcMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_copy(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
						func = _module->GetFunctionByName(String("set_"_s + decl.Declaration).data());
						if (func != nullptr) {
							auto entry = _funcMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
					} else {
						std::int32_t typeId = _module->GetTypeIdByDecl(decl.ParentClass.data());
						auto it = _classMetadataMap.find(typeId);
						if (it == _classMetadataMap.end()) {
							it = _classMetadataMap.emplace(typeId, ClassMetadata()).first;
						}

						asITypeInfo* type = _engine->GetTypeInfoById(typeId);
						asIScriptFunction* func = type->GetMethodByName(String("get_" + decl.Declaration).data());
						if (func != nullptr) {
							auto entry = it->second.FuncMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_copy(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
						func = type->GetMethodByName(String("set_" + decl.Declaration).data());
						if (func != nullptr) {
							auto entry = it->second.FuncMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
					}
					break;
				}
				case MetadataType::Variable: {
					if (decl.ParentClass.empty()) {
						std::int32_t varIdx = _module->GetGlobalVarIndexByName(decl.Declaration.data());
						if (varIdx >= 0) {
							auto entry = _varMetadataMap.emplace(varIdx, Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
					} else {
						std::int32_t typeId = _module->GetTypeIdByDecl(decl.ParentClass.data());
						auto it = _classMetadataMap.find(typeId);
						if (it == _classMetadataMap.end()) {
							it = _classMetadataMap.emplace(typeId, ClassMetadata()).first;
						}

						asITypeInfo* objectType = _engine->GetTypeInfoById(typeId);
						std::int32_t idx = -1;
						for (std::uint32_t i = 0; i < (std::uint32_t)objectType->GetPropertyCount(); i++) {
							const char* name;
							objectType->GetProperty(i, &name);
							if (decl.Declaration == StringView(name)) {
								idx = i;
								break;
							}
						}

						if (idx >= 0) {
							auto entry = it->second.VarMetadataMap.emplace(idx, Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						}
					}
					break;
				}
				case MetadataType::FunctionOrVariable: {
					if (decl.ParentClass .empty()) {
						std::int32_t varIdx = _module->GetGlobalVarIndexByName(decl.Name.data());
						if (varIdx >= 0) {
							auto entry = _varMetadataMap.emplace(varIdx, Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						} else {
							asIScriptFunction* func = _module->GetFunctionByDecl(decl.Declaration.data());
							if (func != nullptr) {
								auto entry = _funcMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
								std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
							}
						}
					} else {
						std::int32_t typeId = _module->GetTypeIdByDecl(decl.ParentClass.data());
						auto it = _classMetadataMap.find(typeId);
						if (it == _classMetadataMap.end()) {
							it = _classMetadataMap.emplace(typeId, ClassMetadata()).first;
						}

						asITypeInfo* objectType = _engine->GetTypeInfoById(typeId);
						std::int32_t idx = -1;
						for (std::uint32_t i = 0; i < (std::uint32_t)objectType->GetPropertyCount(); i++) {
							const char* name;
							objectType->GetProperty(i, &name);
							if (decl.Name == StringView(name)) {
								idx = i;
								break;
							}
						}

						if (idx >= 0) {
							auto entry = it->second.VarMetadataMap.emplace(idx, Array<String>(NoInit, decl.Metadata.size())).first;
							std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
						} else {
							asITypeInfo* type = _engine->GetTypeInfoById(typeId);
							asIScriptFunction* func = type->GetMethodByDecl(decl.Declaration.data());
							if (func != nullptr) {
								auto entry = it->second.FuncMetadataMap.emplace(func->GetId(), Array<String>(NoInit, decl.Metadata.size())).first;
								std::uninitialized_move(decl.Metadata.begin(), decl.Metadata.end(), entry->second.data());
							}
						}
					}
					break;
				}
			}
		}
		_module->SetDefaultNamespace("");

		// _foundDeclarations is not needed anymore
		_foundDeclarations.clear();

		return ScriptBuildResult::Success;
	}

	void ScriptLoader::SetContextType(ScriptContextType value)
	{
		_scriptContextType = value;
	}

	std::int32_t ScriptLoader::ExcludeCode(String& scriptContent, std::int32_t pos)
	{
		std::int32_t scriptSize = (std::int32_t)scriptContent.size();
		std::uint32_t len = 0;
		std::int32_t nested = 0;

		while (pos < scriptSize) {
			_engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			if (scriptContent[pos] == '#') {
				scriptContent[pos] = ' ';
				pos++;

				_engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);

				StringView token = scriptContent.slice(pos, pos + len);

				if (token == "if"_s) {
					nested++;
				} else if (token == "endif"_s) {
					if (nested-- == 0) {
						for (std::uint32_t i = pos; i < pos + len; i++) {
							if (scriptContent[i] != '\n') {
								scriptContent[i] = ' ';
							}
						}

						pos += len;
						break;
					}
				}
			}
			if (scriptContent[pos] != '\n') {
				for (std::uint32_t i = pos; i < pos + len; i++) {
					if (scriptContent[i] != '\n') {
						scriptContent[i] = ' ';
					}
				}
			}
			pos += len;
		}

		return pos;
	}

	std::int32_t ScriptLoader::SkipStatement(String& scriptContent, std::int32_t pos)
	{
		std::int32_t scriptSize = (std::int32_t)scriptContent.size();
		std::uint32_t len = 0;

		// Skip until ; or { whichever comes first
		while (pos < scriptSize && scriptContent[pos] != ';' && scriptContent[pos] != '{') {
			_engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			pos += len;
		}

		// Skip entire statement block
		if (pos < scriptSize && scriptContent[pos] == '{') {
			pos += 1;

			std::int32_t level = 1;
			while (level > 0 && pos < scriptSize) {
				asETokenClass t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
				if (t == asTC_KEYWORD) {
					if (scriptContent[pos] == '{') {
						level++;
					} else if (scriptContent[pos] == '}') {
						level--;
					}
				} else if (t == asTC_IDENTIFIER) {
					// Convert all length() function calls to virtual properties for JJ2+ backward compatibility
					auto identifier = MutableStringView(&scriptContent[pos], len);
					if (identifier == "length"_s) {
						std::int32_t pos1 = pos + len;
						std::int32_t pos2 = pos1;
						std::uint32_t len2 = 0;
						asETokenClass t2 = asTC_UNKNOWN;
						while (pos2 < scriptSize) {
							t2 = _engine->ParseToken(&scriptContent[pos2], scriptSize - pos2, &len2);
							if (t2 != asTC_COMMENT && t2 != asTC_WHITESPACE) {
								break;
							}
							pos2 += len2;
						}
						if (t2 == asTC_KEYWORD && scriptContent[pos2] == '(') {
							std::int32_t pos3 = pos2 + len2;
							std::uint32_t len3 = 0;
							asETokenClass t3 = asTC_UNKNOWN;
							while (pos3 < scriptSize) {
								t3 = _engine->ParseToken(&scriptContent[pos3], scriptSize - pos3, &len3);
								if (t3 != asTC_COMMENT && t3 != asTC_WHITESPACE) {
									break;
								}
								pos3 += len3;
							}
							if (t3 == asTC_KEYWORD && scriptContent[pos3] == ')') {
								pos3 += len3;
								std::memset(&scriptContent[pos + len], ' ', pos3 - pos1);
								pos = pos3;
								continue;
							}
						}
					}
				}

				pos += len;
			}
		} else {
			pos += 1;
		}
		return pos;
	}

	std::int32_t ScriptLoader::ExtractMetadata(MutableStringView scriptContent, std::int32_t pos, SmallVectorImpl<String>& metadata)
	{
		std::int32_t scriptSize = (std::int32_t)scriptContent.size();

		metadata.clear();

		// Extract all metadata, they can be separated by whitespace and comments
		while (true) {
			Array<char> metadataString;

			// Overwrite the metadata with space characters to allow compilation
			scriptContent[pos++] = ' ';

			std::int32_t level = 1;
			std::uint32_t len = 0;
			while (level > 0 && pos < scriptSize) {
				asETokenClass t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
				if (t == asTC_KEYWORD) {
					if (scriptContent[pos] == '[') {
						level++;
					} else if (scriptContent[pos] == ']') {
						level--;
					}
				}

				// Copy the metadata to our buffer
				if (level > 0) {
					arrayAppend(metadataString, arrayView((const char*)&scriptContent[pos], len));
				}

				if (t != asTC_WHITESPACE) {
					for (uint32_t i = pos; i < pos + len; i++) {
						if (scriptContent[i] != '\n') {
							scriptContent[i] = ' ';
						}
					}
				}

				pos += len;
			}

			metadata.emplace_back(String(std::move(metadataString)));

			// Check for more metadata, possibly separated by comments
			asETokenClass t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			while (t == asTC_COMMENT || t == asTC_WHITESPACE) {
				pos += len;
				t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			}

			if (scriptContent[pos] != '[') {
				break;
			}
		}

		return pos;
	}

	std::int32_t ScriptLoader::ExtractDeclaration(StringView scriptContent, std::int32_t pos, String& name, String& declaration, MetadataType& type)
	{
		std::int32_t scriptSize = (std::int32_t)scriptContent.size();
		std::int32_t start = pos;

		declaration = {};
		type = MetadataType::Unknown;

		StringView token;
		std::uint32_t len = 0;
		asETokenClass t = asTC_WHITESPACE;

		// Skip white spaces, comments and leading decorators
		do {
			pos += len;
			t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
			token = scriptContent.sliceSize(pos, len);
		} while (t == asTC_WHITESPACE || t == asTC_COMMENT ||
				  token == "private"_s || token == "protected"_s || token == "shared"_s ||
				  token == "external"_s || token == "final"_s || token == "abstract"_s);

		// We're expecting, either a class, interface, function, or variable declaration
		if (t == asTC_KEYWORD || t == asTC_IDENTIFIER) {
			token = scriptContent.sliceSize(pos, len);
			if (token == "interface"_s || token == "class"_s || token == "enum"_s) {
				do {
					pos += len;
					t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
				} while (t == asTC_WHITESPACE || t == asTC_COMMENT);

				if (t == asTC_IDENTIFIER) {
					type = MetadataType::Type;
					declaration = scriptContent.slice(pos, pos + len);
					pos += len;
					return pos;
				}
			} else {
				// For function declarations, store everything up to the start of the 
				// statement block, except for succeeding decorators (final, override, etc)
				// For variable declaration store just the name as there can only be one

				// We'll only know if the declaration is a variable or function declaration
				// when we see the statement block, or absense of a statement block.
				bool hasParenthesis = false;
				std::int32_t nestedParenthesis = 0;
				declaration += scriptContent.slice(pos, pos + len);
				pos += len;
				for (; pos < scriptSize;) {
					t = _engine->ParseToken(&scriptContent[pos], scriptSize - pos, &len);
					token = scriptContent.sliceSize(pos, len);
					if (t == asTC_KEYWORD) {
						if (token == "{"_s && nestedParenthesis == 0) {
							if (hasParenthesis) {
								type = MetadataType::Function;
							} else {
								declaration = name;
								type = MetadataType::VirtualProperty;
							}
							return pos;
						}
						if ((token == "="_s && !hasParenthesis) || token == ";"_s) {
							if (hasParenthesis) {
								// The declaration is ambigous, it can be variable with initialization, or function prototype
								type = MetadataType::FunctionOrVariable;
							} else {
								declaration = name;
								type = MetadataType::Variable;
							}
							return pos;
						} else if (token == "("_s) {
							nestedParenthesis++;

							// This is the first parenthesis we encounter. If the parenthesis isn't followed
							// by a statement block, then this is a variable declaration, in which case we
							// should only store the type and name of the variable, not the initialization parameters.
							hasParenthesis = true;
						} else if (token == ")"_s) {
							nestedParenthesis--;
						}
					} else if (t == asTC_IDENTIFIER) {
						name = token;
					}

					if (!hasParenthesis || nestedParenthesis > 0 || t != asTC_IDENTIFIER || (token != "final"_s && token != "override"_s)) {
						declaration += token;
					}
					pos += len;
				}
			}
		}

		return start;
	}

	ArrayView<String> ScriptLoader::GetMetadataForType(std::int32_t typeId)
	{
		auto it = _typeMetadataMap.find(typeId);
		if (it != _typeMetadataMap.end()) {
			return it->second;
		}
		return {};
	}

	ArrayView<String> ScriptLoader::GetMetadataForFunction(asIScriptFunction* func)
	{
		if (func != nullptr) {
			auto it = _funcMetadataMap.find(func->GetId());
			if (it != _funcMetadataMap.end()) {
				return it->second;
			}
		}
		return {};
	}

	ArrayView<String> ScriptLoader::GetMetadataForVariable(std::int32_t varIdx)
	{
		auto it = _varMetadataMap.find(varIdx);
		if (it != _varMetadataMap.end()) {
			return it->second;
		}
		return {};
	}

	ArrayView<String> ScriptLoader::GetMetadataForTypeProperty(std::int32_t typeId, std::int32_t varIdx)
	{
		auto typeIt = _classMetadataMap.find(typeId);
		if (typeIt == _classMetadataMap.end()) {
			return {};
		}
		auto propIt = typeIt->second.VarMetadataMap.find(varIdx);
		if (propIt == typeIt->second.VarMetadataMap.end()) {
			return {};
		}
		return propIt->second;
	}

	ArrayView<String> ScriptLoader::GetMetadataForTypeMethod(std::int32_t typeId, asIScriptFunction* method)
	{
		if (method == nullptr) {
			return {};
		}
		auto typeIt = _classMetadataMap.find(typeId);
		if (typeIt == _classMetadataMap.end()) {
			return {};
		}
		auto methodIt = typeIt->second.FuncMetadataMap.find(method->GetId());
		if (methodIt == typeIt->second.FuncMetadataMap.end()) {
			return {};
		}
		return methodIt->second;
	}

	String ScriptLoader::MakeRelativePath(StringView path, StringView relativeToFile)
	{
		if (path.empty() || path.size() > fs::MaxPathLength) return {};

		char result[fs::MaxPathLength + 1];
		std::size_t length = 0;

		if (path[0] == '/' || path[0] == '\\') {
			// Absolute path from "Content" directory
			const char* src = &path[1];
			const char* srcLast = src;

			auto contentPath = ContentResolver::Get().GetContentPath();
			std::memcpy(result, contentPath.data(), contentPath.size());
			char* dst = result + contentPath.size();
			if (*(dst - 1) == '/' || *(dst - 1) == '\\') {
				dst--;
			}
			char* dstStart = dst;
			char* dstLast = dstStart;

			while (true) {
				bool end = (src - path.begin()) >= path.size();
				if (end || *src == '/' || *src == '\\') {
					if (src > srcLast) {
						size_t length = src - srcLast;
						if (length == 1 && srcLast[0] == '.') {
							// Ignore this
						} else if (length == 2 && srcLast[0] == '.' && srcLast[1] == '.') {
							if (dst != dstStart) {
								if (dst == dstLast && dstStart <= dstLast - 1) {
									dstLast--;
									while (dstStart <= dstLast) {
										if (*dstLast == '/' || *dstLast == '\\') {
											break;
										}
										dstLast--;
									}
								}
								dst = dstLast;
							}
						} else {
							dstLast = dst;

							if ((dst - result) + (sizeof(fs::PathSeparator) - 1) + (src - srcLast) >= fs::MaxPathLength) {
								return {};
							}

							if (dst != result) {
								std::memcpy(dst, fs::PathSeparator, sizeof(fs::PathSeparator) - 1);
								dst += sizeof(fs::PathSeparator) - 1;
							}
							std::memcpy(dst, srcLast, src - srcLast);
							dst += src - srcLast;
						}
					}
					if (end) {
						break;
					}
					srcLast = src + 1;
				}
				src++;
			}
			length = dst - result;
		} else {
			// Relative path to script file
			String dirPath = fs::GetDirectoryName(relativeToFile);
			if (dirPath.empty()) return {};

			const char* src = &path[0];
			const char* srcLast = src;

			std::memcpy(result, dirPath.data(), dirPath.size());
			char* dst = result + dirPath.size();
			if (*(dst - 1) == '/' || *(dst - 1) == '\\') {
				dst--;
			}
			char* searchBack = dst - 2;

			char* dstStart = dst;
			while (result <= searchBack) {
				if (*searchBack == '/' || *searchBack == '\\') {
					dstStart = searchBack + 1;
					break;
				}
				searchBack--;
			}
			char* dstLast = dstStart;

			while (true) {
				bool end = (src - path.begin()) >= path.size();
				if (end || *src == '/' || *src == '\\') {
					if (src > srcLast) {
						size_t length = src - srcLast;
						if (length == 1 && srcLast[0] == '.') {
							// Ignore this
						} else if (length == 2 && srcLast[0] == '.' && srcLast[1] == '.') {
							if (dst != dstStart) {
								if (dst == dstLast && dstStart <= dstLast - 1) {
									dstLast--;
									while (dstStart <= dstLast) {
										if (*dstLast == '/' || *dstLast == '\\') {
											break;
										}
										dstLast--;
									}
								}
								dst = dstLast;
							}
						} else {
							dstLast = dst;

							if ((dst - result) + (sizeof(fs::PathSeparator) - 1) + (src - srcLast) >= fs::MaxPathLength) {
								return {};
							}

							if (dst != result) {
								std::memcpy(dst, fs::PathSeparator, sizeof(fs::PathSeparator) - 1);
								dst += sizeof(fs::PathSeparator) - 1;
							}
							std::memcpy(dst, srcLast, src - srcLast);
							dst += src - srcLast;
						}
					}
					if (end) {
						break;
					}
					srcLast = src + 1;
				}
				src++;
			}
			length = dst - result;
		}

		return String(result, length);
	}

	asIScriptContext* ScriptLoader::RequestContextCallback(asIScriptEngine* engine, void* param)
	{
		// Check if there is a free context available in the pool
		auto _this = static_cast<ScriptLoader*>(param);
		if (!_this->_contextPool.empty()) {
			return _this->_contextPool.pop_back_val();
		} else {
			// No free context was available so we'll have to create a new one
			return engine->CreateContext();
		}
	}

	void ScriptLoader::ReturnContextCallback(asIScriptEngine* engine, asIScriptContext* ctx, void* param)
	{
		// Unprepare the context to free any objects it may still hold (e.g. return value)
		// This must be done before making the context available for re-use, as the clean
		// up may trigger other script executions, e.g. if a destructor needs to call a function.
		ctx->Unprepare();

		// Place the context into the pool for when it will be needed again
		auto _this = static_cast<ScriptLoader*>(param);
		_this->_contextPool.push_back(ctx);
	}

	void ScriptLoader::Message(const asSMessageInfo& msg)
	{
		TraceLevel level;
		switch (msg.type) {
			case asMSGTYPE_ERROR: level = TraceLevel::Error; break;
			case asMSGTYPE_WARNING: level = TraceLevel::Warning; break;
			default: level = TraceLevel::Info; break;
		}

		if (msg.section != nullptr && msg.section[0] != '\0') {
			__DEATH_TRACE(level, "AS!", "{}:{}({}): {}", msg.section, msg.row, msg.col, msg.message);
		} else {
			__DEATH_TRACE(level, "AS!", "{}", msg.message);
		}
	}
}

#endif