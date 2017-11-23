// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include <CrySchematyc2/Script/Schematyc_IScriptFile.h>

#include "Script/Schematyc_ScriptElementBase.h"
#include "Script/Schematyc_ScriptUserDocumentation.h"
#include "Script/Schematyc_ScriptVariableDeclaration.h"

namespace Schematyc2
{
	class CScriptAbstractInterface : public CScriptElementBase<IScriptAbstractInterface>
	{
	public:

		// #SchematycTODO : Create two separate constructors: one default (before loading) and one for when element is created in editor.
		CScriptAbstractInterface(IScriptFile& file, const SGUID& guid = SGUID(), const SGUID& scopeGUID = SGUID(), const char* szName = nullptr);

		// IScriptElement
		virtual EAccessor GetAccessor() const override;
		virtual SGUID GetGUID() const override;
		virtual SGUID GetScopeGUID() const override;
		virtual bool SetName(const char* szName) override;
		virtual const char* GetName() const override;
		virtual void EnumerateDependencies(const ScriptDependancyEnumerator& enumerator) const override;
		virtual void Refresh(const SScriptRefreshParams& params) override;
		virtual void Serialize(Serialization::IArchive& archive) override;
		virtual void RemapGUIDs(IGUIDRemapper& guidRemapper) override;
		// ~IScriptElement

		// IScriptAbstractInterface
		virtual const char* GetAuthor() const override;
		virtual const char* GetDescription() const override;
		// ~IScriptAbstractInterface

	private:

		SGUID                    m_guid;
		SGUID                    m_scopeGUID;
		string                   m_name;
		SScriptUserDocumentation m_userDocumentation;
	};

	DECLARE_SHARED_POINTERS(CScriptAbstractInterface)
}