#include "Events.h"
#include "Graphics.h"

namespace Events
{
	Manager* Manager::GetSingleton()
	{
		static Manager singleton;
		return std::addressof(singleton);
	}

	void Manager::Register()
	{
		logger::info("{:*^30}", "EVENTS");

		bool playerCombat = false;
		bool npcCombat = false;
		bool homeHide = false;
		bool dialogue = false;
		bool useHotKey = false;

		Settings::GetSingleton()->ForEachSlot([&](const SlotData& a_slotData) {
			if (playerCombat && npcCombat && homeHide && dialogue && useHotKey) {
				return false;
			}

			auto& [hotKey, hide, unhide, slots] = a_slotData;

			if (!playerCombat && unhide.combat.CanDoPlayerToggle()) {
				playerCombat = true;

				std::array targets{
					std::make_pair(RELOCATION_ID(45569, 46869), 0xB0),                  // CombatManager::Update
					std::make_pair(RELOCATION_ID(39465, 40542), OFFSET(0x193, 0x17C)),  // PlayerCharacter::FinishLoadGame
					std::make_pair(RELOCATION_ID(39378, 40450), OFFSET(0x183, 0x18C)),  // PlayerCharacter::UpdateFreeCamera
				};

				for (const auto& [id, offset] : targets) {
					REL::Relocation<std::uintptr_t> target{ id, offset };
					stl::write_thunk_call<UpdatePlayerCombat>(target.address());
				}

				logger::info("Registered for player combat hook");
			}
			if (!npcCombat && unhide.combat.CanDoFollowerToggle()) {
				npcCombat = true;
				if (const auto scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
					scripts->AddEventSink<RE::TESCombatEvent>(GetSingleton());

					logger::info("Registered for NPC combat");
				}
			}
			if (!homeHide && hide.home.toggle != Toggle::Type::kDisabled) {
				homeHide = true;
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					player->AddEventSink<RE::BGSActorCellEvent>(GetSingleton());

					logger::info("Registered for player cell change event");
				}
			}
			if (!dialogue && hide.dialogue.toggle != Toggle::Type::kDisabled) {
				dialogue = true;
				if (const auto menuMgr = RE::UI::GetSingleton()) {
					menuMgr->AddEventSink<RE::MenuOpenCloseEvent>(GetSingleton());

					logger::info("Registered for dialogue menu event");
				}
			}
			if (!useHotKey && hotKey.key != Key::kNone && hotKey.type.toggle != Toggle::Type::kDisabled) {
				useHotKey = true;
				if (const auto inputMgr = RE::BSInputDeviceManager::GetSingleton()) {
					inputMgr->AddEventSink(GetSingleton());

					logger::info("Registered for hotkey event");
				}
			}

			return true;
		});
	}

	EventResult Manager::ProcessEvent(const RE::TESCombatEvent* evn, RE::BSTEventSource<RE::TESCombatEvent>*)
	{
		if (!evn || !evn->actor) {
			return EventResult::kContinue;
		}

		const auto actor = evn->actor->As<RE::Actor>();
		if (!actor) {
			return EventResult::kContinue;
		}

		const auto can_toggle = [&](const SlotData& a_slotData) {
			return a_slotData.unhide.combat.CanDoToggle(actor);
		};

		switch (*evn->newState) {
		case RE::ACTOR_COMBAT_STATE::kCombat:
			Graphics::ToggleActorEquipment(actor, can_toggle, Graphics::State::kUnhide);
			break;
		case RE::ACTOR_COMBAT_STATE::kNone:
			Graphics::ToggleActorEquipment(actor, can_toggle, Graphics::State::kHide);
			break;
		default:
			break;
		}

		return EventResult::kContinue;
	}

	EventResult Manager::ProcessEvent(const RE::BGSActorCellEvent* a_evn, RE::BSTEventSource<RE::BGSActorCellEvent>*)
	{
		if (!a_evn || a_evn->flags == RE::BGSActorCellEvent::CellFlag::kLeave) {
			return EventResult::kContinue;
		}

		const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_evn->cellID);
		if (!cell) {
			return EventResult::kContinue;
		}

		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return EventResult::kContinue;
		}

		constexpr auto is_cell_home = [&]() {
			if (cell->IsInteriorCell()) {
				if (const auto loc = cell->GetLocation(); loc) {
					return loc->HasKeywordString(PlayerHome) || loc->HasKeywordString(Inn);
				}
			}
			return false;
		};

		bool result = false;
		if (is_cell_home()) {
			playerInHouse = true;
			result = true;
		} else if (playerInHouse) {
			playerInHouse = false;
			result = true;
		}

		if (result) {
			Graphics::ToggleActorEquipment(
				player, [&](const SlotData& a_slotData) {
					return a_slotData.hide.home.CanDoPlayerToggle();
				},
				playerInHouse ? Graphics::State::kHide : Graphics::State::kUnhide);

			Graphics::ToggleFollowerEquipment([](const SlotData& a_slotData) {
				return a_slotData.hide.home.CanDoFollowerToggle();
			},
				playerInHouse ? Graphics::State::kHide : Graphics::State::kUnhide);
		}

		return EventResult::kContinue;
	}

	EventResult Manager::ProcessEvent(RE::InputEvent* const* a_evn, RE::BSTEventSource<RE::InputEvent*>*)
	{
		using InputType = RE::INPUT_EVENT_TYPE;
		using Keyboard = RE::BSWin32KeyboardDevice::Key;

		if (!a_evn) {
			return EventResult::kContinue;
		}

		if (const auto UI = RE::UI::GetSingleton(); UI->GameIsPaused() || UI->IsModalMenuOpen() || UI->IsApplicationMenuOpen()) {
			return EventResult::kContinue;
		}

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return EventResult::kContinue;
		}

		for (auto event = *a_evn; event; event = event->next) {
			if (event->eventType != InputType::kButton) {
				continue;
			}

			const auto button = static_cast<RE::ButtonEvent*>(event);
			if (!button->IsDown() || button->device != RE::INPUT_DEVICE::kKeyboard) {
				continue;
			}

			const auto key = static_cast<Key>(button->idCode);
			Graphics::ToggleAllEquipment([&](RE::Actor* a_actor, const SlotData& a_slotData) {
				auto& [hotKey, hide, unhide, slots] = a_slotData;
				return hotKey.key == key && hotKey.type.CanDoToggle(a_actor);
			});
		}

		return EventResult::kContinue;
	}

	EventResult Manager::ProcessEvent(const RE::MenuOpenCloseEvent* a_evn, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		if (!a_evn) {
			return EventResult::kContinue;
		}

		if (a_evn->menuName == RE::DialogueMenu::MENU_NAME) {
			if (const auto player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) {
				Graphics::ToggleActorEquipment(
					player, [](const SlotData& a_slotData) {
						return a_slotData.hide.dialogue.CanDoPlayerToggle();
					},
					static_cast<Graphics::State>(a_evn->opening));

				const auto dialogueTarget = RE::MenuTopicManager::GetSingleton()->speaker.get();

				if (const auto dialogueTargetActor = dialogueTarget ? dialogueTarget->As<RE::Actor>() : nullptr) {
					Graphics::ToggleActorEquipment(
						dialogueTargetActor, [&](const SlotData& a_slotData) {
							return a_slotData.hide.dialogue.CanDoToggle(dialogueTargetActor);
						},
						static_cast<Graphics::State>(a_evn->opening));
				}
			}
		}

		return EventResult::kContinue;
	}

	void Manager::UpdatePlayerCombat::thunk(RE::PlayerCharacter* a_this, float a_delta)
	{
		func(a_this, a_delta);

		const bool isInCombat = a_this->IsInCombat();

		if (isInCombat) {
			if (!playerInCombat) {
				playerInCombat = true;
				Graphics::ToggleActorEquipment(
					a_this, [](const SlotData& a_slotData) {
						return a_slotData.unhide.combat.CanDoPlayerToggle();
					},
					Graphics::State::kUnhide);
			}
		} else {
			if (playerInCombat) {
				playerInCombat = false;
				Graphics::ToggleActorEquipment(
					a_this, [](const SlotData& a_slotData) {
						return a_slotData.unhide.combat.CanDoPlayerToggle();
					},
					Graphics::State::kHide);
			}
		}
	}
}
