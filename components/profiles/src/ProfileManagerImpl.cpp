/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "ProfileManagerImpl.h"

#include <dpapi.h>
#include <ShlObj.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#pragma comment(lib, "crypt32.lib")

void ProfileManagerImpl::Initialize()
{
	// load stored profiles
	LoadStoredProfiles();

	// initialize profiles from profile suggestion providers
	for (auto&& provider : m_suggestionProviders)
	{
		provider->GetProfiles([=] (fwRefContainer<Profile> profile)
		{
			fwRefContainer<ProfileImpl> profileImpl = profile;

			// find a matching profile - only add it to the list if none match any existing profile
			bool matchesExistingProfile = false;
			fwRefContainer<ProfileImpl> matchingProfile;

			size_t hashKey = 0;

			for (int i = 0; i < profileImpl->GetNumIdentifiers(); i++)
			{
				ProfileIdentifier identifier = profileImpl->GetIdentifierInternal(i);
				
				for (auto&& profile : m_profiles)
				{
					auto& thatProfile = profile.second;

					for (int j = 0; j < thatProfile->GetNumIdentifiers(); j++)
					{
						ProfileIdentifier thatIdentifier = thatProfile->GetIdentifierInternal(j);

						if (identifier == thatIdentifier)
						{
							matchesExistingProfile = true;
							matchingProfile = thatProfile;
							break;
						}
					}

					if (matchesExistingProfile)
					{
						break;
					}
				}

				hashKey ^= 3 * std::hash<ProfileIdentifier>()(identifier);
			}

			// clip the hash key to 32 bits
			hashKey &= UINT32_MAX;

			// mark the profile as suggestion
			profileImpl->SetIsSuggestion(true);

			// set the internal identifier
			profileImpl->SetInternalIdentifier(hashKey);

			if (!matchesExistingProfile)
			{
				m_profiles[hashKey] = profileImpl;
				m_profileIndices.push_back(hashKey);
			}
			else
			{
				matchingProfile->SetDisplayName(profileImpl->GetDisplayName());
				matchingProfile->SetTileURI(profileImpl->GetTileURI());
			}
		});
	}
}

void ProfileManagerImpl::LoadStoredProfiles()
{
	PWSTR appDataPath;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
	{
		// create the directory if not existent
		std::wstring cfxPath = std::wstring(appDataPath) + L"\\CitizenFX";
		CreateDirectory(cfxPath.c_str(), nullptr);

		// open and read the profile file
		std::wstring profilePath = cfxPath + L"\\profiles.json";

		if (FILE* profileFile = _wfopen(profilePath.c_str(), L"rb"))
		{
			std::vector<uint8_t> profileFileData;
			int pos;

			// get the file length
			fseek(profileFile, 0, SEEK_END);
			pos = ftell(profileFile);
			fseek(profileFile, 0, SEEK_SET);

			// resize the buffer
			profileFileData.resize(pos);

			// read the file and close it
			fread(&profileFileData[0], 1, pos, profileFile);

			fclose(profileFile);

			// decrypt the stored data - setup blob
			DATA_BLOB cryptBlob;
			cryptBlob.pbData = &profileFileData[0];
			cryptBlob.cbData = profileFileData.size();

			DATA_BLOB outBlob;

			// call DPAPI
			if (CryptUnprotectData(&cryptBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob))
			{
				// parse the profile list
				ParseStoredProfiles(std::string(reinterpret_cast<char*>(outBlob.pbData), outBlob.cbData));

				// free the out data
				LocalFree(outBlob.pbData);
			}
		}
		
		CoTaskMemFree(appDataPath);
	}
}

void ProfileManagerImpl::ParseStoredProfiles(const std::string& profileList)
{
	rapidjson::Document document;
	document.Parse(profileList.c_str());

	if (!document.HasParseError())
	{
		if (document.IsObject())
		{
			if (document.HasMember("profiles") && document["profiles"].IsArray())
			{
				// loop through each profile
				auto& profiles = document["profiles"];

				std::for_each(profiles.Begin(), profiles.End(), [&] (rapidjson::Value& value)
				{
					if (!value.IsObject())
					{
						return;
					}

					// get display name
					if (!value.HasMember("displayName"))
					{
						return;
					}

					auto& displayNameMember = value["displayName"];

					if (!displayNameMember.IsString())
					{
						return;
					}

					// get tile URI
					if (!value.HasMember("tileUri"))
					{
						return;
					}

					auto& tileUriMember = value["tileUri"];

					if (!tileUriMember.IsString())
					{
						return;
					}

					// get identifier pairs
					if (!value.HasMember("identifiers"))
					{
						return;
					}

					auto& identifiersMember = value["identifiers"];

					if (!identifiersMember.IsArray())
					{
						return;
					}

					// enumerate identifier pairs
					std::vector<ProfileIdentifier> identifiers;
					size_t hashKey = 0;

					std::for_each(identifiersMember.Begin(), identifiersMember.End(), [&] (rapidjson::Value& identifierMember)
					{
						if (!identifierMember.IsArray())
						{
							return;
						}

						if (identifierMember.Size() != 2)
						{
							return;
						}

						auto& left = identifierMember[(rapidjson::SizeType)0]; // regular 0 is ambiguous as it could be a pointer
						auto& right = identifierMember[1];

						if (!left.IsString())
						{
							return;
						}

						if (!right.IsString())
						{
							return;
						}

						// add the identifier
						ProfileIdentifier identifier = std::make_pair(left.GetString(), right.GetString());

						identifiers.push_back(identifier);
						hashKey ^= 3 * std::hash<ProfileIdentifier>()(identifier);
					});

					// create an implemented profile
					fwRefContainer<ProfileImpl> profile = new ProfileImpl();
					profile->SetDisplayName(std::string(displayNameMember.GetString(), displayNameMember.GetStringLength()));
					profile->SetTileURI(std::string(tileUriMember.GetString(), tileUriMember.GetStringLength()));

					profile->SetInternalIdentifier(hashKey);
					profile->SetIdentifiers(identifiers);

					m_profiles[hashKey] = profile;
					m_profileIndices.push_back(hashKey);
				});
			}
		}
	}
}

void ProfileManagerImpl::SaveStoredProfiles(const std::string& savedList)
{
	// encrypt the actual string
	DATA_BLOB cryptBlob;
	cryptBlob.pbData = reinterpret_cast<uint8_t*>(const_cast<char*>(savedList.c_str()));
	cryptBlob.cbData = savedList.size();

	DATA_BLOB outBlob;

	if (CryptProtectData(&cryptBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob))
	{
		PWSTR appDataPath;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
		{
			// create the directory if not existent
			std::wstring cfxPath = std::wstring(appDataPath) + L"\\CitizenFX";
			CreateDirectory(cfxPath.c_str(), nullptr);

			// open and read the profile file
			std::wstring profilePath = cfxPath + L"\\profiles.json";

			if (FILE* profileFile = _wfopen(profilePath.c_str(), L"wb"))
			{
				fwrite(outBlob.pbData, 1, outBlob.cbData, profileFile);
				fclose(profileFile);
			}

			CoTaskMemFree(appDataPath);
		}

		LocalFree(outBlob.pbData);
	}
}

void ProfileManagerImpl::UpdateStoredProfiles()
{
	rapidjson::Document document;
	document.SetObject();

	rapidjson::Value profiles;
	profiles.SetArray();

	for (auto& profilePair : m_profiles)
	{
		fwRefContainer<ProfileImpl> profile = profilePair.second;
		rapidjson::Value profileData;
		profileData.SetObject();
		
		profileData.AddMember("displayName", rapidjson::Value(profile->GetDisplayName(), document.GetAllocator()).Move(), document.GetAllocator());
		profileData.AddMember("tileUri", rapidjson::Value(profile->GetTileURI(), document.GetAllocator()).Move(), document.GetAllocator());

		rapidjson::Value identifiers;
		identifiers.SetArray();

		for (int i = 0; i < profile->GetNumIdentifiers(); i++)
		{
			ProfileIdentifier identifier = profile->GetIdentifierInternal(i);

			rapidjson::Value identifierValue;
			identifierValue.SetArray();

			identifierValue.PushBack(rapidjson::Value(identifier.first.c_str(), document.GetAllocator()).Move(), document.GetAllocator());
			identifierValue.PushBack(rapidjson::Value(identifier.second.c_str(), document.GetAllocator()).Move(), document.GetAllocator());

			identifiers.PushBack(identifierValue, document.GetAllocator());
		}

		profileData.AddMember("identifiers", identifiers, document.GetAllocator());

		profiles.PushBack(profileData, document.GetAllocator());
	}

	document.AddMember("profiles", profiles, document.GetAllocator());

	rapidjson::StringBuffer stringBuffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(stringBuffer);
	
	document.Accept(writer);

	SaveStoredProfiles(std::string(stringBuffer.GetString(), stringBuffer.GetSize()));
}

void ProfileManagerImpl::AddSuggestionProvider(fwRefContainer<ProfileSuggestionProvider> provider)
{
	m_suggestionProviders.push_back(provider);
}

void ProfileManagerImpl::AddIdentityProvider(fwRefContainer<ProfileIdentityProvider> provider)
{
	m_identityProviders[provider->GetIdentifierKey()] = provider;
}

int ProfileManagerImpl::GetNumProfiles()
{
	return m_profileIndices.size();
}

fwRefContainer<Profile> ProfileManagerImpl::GetProfile(int index)
{
	return m_profiles[m_profileIndices[index]];
}

concurrency::task<ProfileTaskResult> ProfileManagerImpl::AddProfile(fwRefContainer<Profile> profile)
{
	ProfileTaskResult result;

	return concurrency::task_from_result<ProfileTaskResult>(result);
}

concurrency::task<ProfileTaskResult> ProfileManagerImpl::SetPrimaryProfile(fwRefContainer<Profile> profile)
{
	ProfileTaskResult result;

	return concurrency::task_from_result<ProfileTaskResult>(result);
}

using namespace terminal;

struct VoidFunctionHolder
{
	std::function<void()> func;
};

concurrency::task<ProfileTaskResult> ProfileManagerImpl::SignIn(fwRefContainer<Profile> profile, const std::map<std::string, std::string>& parameters)
{
	// create a result
	concurrency::task_completion_event<ProfileTaskResult> resultEvent;

	// get the profileimpl
	fwRefContainer<ProfileImpl> profileImpl(profile);

	// for each identifier in the profile, get a token
	int numIdentifiers = profile->GetNumIdentifiers();

	// container struct for the function, and a token bag/index
	std::shared_ptr<VoidFunctionHolder> continueIdentifier = std::make_shared<VoidFunctionHolder>();
	std::shared_ptr<TokenBag> tokenBag = std::make_shared<TokenBag>();
	std::shared_ptr<int> idx = std::make_shared<int>(0);

	// when an identifier is returned
	auto identifierCB = [=] (const concurrency::task<ProfileIdentityResult>& resultTask)
	{
		ProfileIdentityResult result = resultTask.get();

		if (result.HasSucceeded())
		{
			tokenBag->AddToken(result.GetTokenType(), result.GetToken());

			(*idx)++;
			continueIdentifier->func();
		}
		else
		{
			resultEvent.set(ProfileTaskResult(false, result.GetError().c_str()));
		}
	};

	// continuation
	continueIdentifier->func = [=] ()
	{
		// if there's any identifiers left to handle
		if (*idx < numIdentifiers)
		{
			auto identifier = profileImpl->GetIdentifierInternal(*idx);

			auto providerIt = m_identityProviders.find(identifier.first);

			if (providerIt != m_identityProviders.end())
			{
				providerIt->second->ProcessIdentity(profile, parameters).then(identifierCB);
			}
		}
		else
		{
			trace("[ProfileManager] Connecting to Terminal...\n");

			// continue signing in to Terminal
			fwRefContainer<IClient> client = terminal::Create<IClient>();

			// set the current Terminal client
			Instance<TerminalClient>::Get()->SetClient(client);

			// connect to the Terminal server
			client->ConnectRemote("layer1://localhost:3036").then([=] (Result<ConnectRemoteDetail> result)
			{
				if (result.HasSucceeded())
				{
					IUser1* user = static_cast<IUser1*>(client->GetUserService(IUser1::InterfaceID).GetDetail());

					user->AuthenticateWithTokenBag(*tokenBag).then([=] (Result<AuthenticateDetail> result)
					{
						if (result.HasSucceeded())
						{
							UpdateStoredProfiles();

							resultEvent.set(ProfileTaskResult(true));
						}
						else
						{
							resultEvent.set(ProfileTaskResult(false, va("Authenticating to Terminal failed - error code %d.", result.GetError())));
						}
					});
				}
				else
				{
					resultEvent.set(ProfileTaskResult(false, va("Connecting to Terminal failed - error code %d.", result.GetError())));
				}
			});
		}
	};
	
	continueIdentifier->func();

	return concurrency::task<ProfileTaskResult>(resultEvent);
}

static ProfileManagerImpl* g_profileManager;

static InitFunction initFunction([] ()
{
	g_profileManager = new ProfileManagerImpl();
	Instance<ProfileManager>::Set(g_profileManager);

	Instance<TerminalClient>::Set(new TerminalClient());
}, -500);

#include <terminal.h>

using namespace terminal;

#include <SteamComponentAPI.h>
#include <base64.h>

static InitFunction initFunctionPost([] ()
{
	g_profileManager->Initialize();
}, 500);