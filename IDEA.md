# Game Vision & Design Ideas

This document organizes the author's goals for the game. It describes the intended experience, not a list of implemented features. The latest decisions take priority over the earlier colony-simulator outline. Suggested implementation stages are proposals; open questions remain open until explicitly decided.

## 1. The Game in One Paragraph

A single-player, character-centered survival RPG and settlement simulation set in a persistent, autonomous world. The player experiences that world through a character, can travel, establish or join settlements, and potentially rise to political leadership. All living beings share an origin in an unnamed primordial white fox, whose dispersed energy allows life and civilizations to develop. People and animals grow through an isekai-inspired system of levels, stats, skill trees, and procedurally combined abilities; sufficiently developed wild creatures can awaken to human-like sapience. Societies own resources, preserve or lose knowledge, pursue interests, and react to events for understandable reasons. The game combines settlement depth and vertical terrain with personal progression, exploration, and useful delegation, while targeting modest computers.

## 2. Inspirations & Desired Experience

| Inspiration | What matters for this game |
| --- | --- |
| RimWorld | Readable colony interaction, survival, technology progression, and societies with uneven technological development. |
| Dwarf Fortress | Z-levels, mining and material use, generated history, visitors and taverns, and interacting environmental systems. |
| Kenshi | A character-based relationship with the world, travel, visibility around controlled people, and freedom to relocate. |
| Tensura | Growth from weakness to extraordinary power, evolving distinctive abilities, conditional unlocks, and influential legendary individuals. |
| The Rising of the Shield Hero | Isekai-style progression and character development; specific mechanics to borrow remain open. |

These are influences, not requirements to reproduce their rules. The latest lore supersedes the earlier mostly-human setting: humans, elves, dwarves, goblins, slimes, and other beings can share this world. Multiple sapient peoples coexist with magic, guns, and technological development; the setting need not reproduce DF fantasy conventions. The primordial fox provides a proposed common origin. Whether “isekai” also means literal reincarnation or arrival from another world remains undecided.

## 3. Confirmed Design Pillars

### A character anchors the experience

The player has a character through whom the world is experienced. This supersedes a purely detached, all-seeing colony-manager perspective. “Point of view” describes the player's relationship to the world; it does not commit to a first-person camera. Whether control can transfer between characters or extend directly to companions is open.

### A living world with consequences

The world must continue functioning without the player directing it. People, settlements, political groups, and creatures have their own goals and constraints. History should emerge before play and continue afterward. Simulation detail can vary with distance; autonomy does not require simulating every distant action at full local resolution.

### Character growth is central

Isekai-style RPG progression is a defining feature, not a late optional addition. People and animals have relevant stats, levels, and growth. Skill trees, conditional unlocks, and procedural skill combinations support distinctive builds and the possibility of becoming extraordinarily powerful.

### Settlement management is mandatory

The player must be able to participate meaningfully in settlement development and governance. Starting a settlement and joining an existing society are both candidate entry routes. Management should work through a generalized social and political model that can represent tribes, kingdoms, and other organizations.

### Events have understandable causes

Raids, migration, trade, and discoveries should arise from circumstances and decisions. Randomness can affect uncertainty and outcomes, but events need a reason. The player should be able to learn that reason through observations, reports, diplomacy, or rumors rather than receiving arbitrary pressure from a hidden difficulty director.

### Freedom to move and build vertically

The player should not be permanently confined to one colony map. Travel, expansion, relocation, and outposts are part of the intended experience. Z-levels enable mining, caves, underground settlements, and shelter below Arctic snow or ice.

### Performance and single-player are foundational

Target good performance on low-end hardware through C++, appropriate data layouts, chunking, and multithreading designed from the beginning. There will be no multiplayer. Time-manipulation and perception abilities should be designed around the single-player experience without multiplayer synchronization constraints.

## 4. Player Experience & Core Loop

A proposed loop that connects the pillars:

1. Explore and survive through the player character.
2. Meet people, learn about local society, and acquire resources and knowledge.
3. Develop abilities, choose a specialization, and discover skill combinations.
4. Establish a home or become part of an existing settlement.
5. Build relationships, influence decisions, and gain responsibility or authority.
6. Delegate work as the settlement grows; respond to consequences and threats.
7. Travel, relocate, expand, or pursue personal and political ambitions.

Settlement growth and personal growth should reinforce each other. Becoming a ruler is a possible trajectory, not an already-decided mandatory objective. Victory conditions and the shape of the endgame remain open.

## 5. Systems & Their Relationships

### World, geography, and history

Generate a world map with terrain, climate, resources, settlements, and a history of interaction. Local regions provide detailed terrain and inhabitants. Wars, migration, collapse, exchange, and notable individuals should leave persistent traces.

Societies should differ because of geography, available resources, people, institutions, knowledge, and history, rather than being only entries in a fixed faction-type list. The common origin of life is now the primordial fox's dispersal, described below. Societies can emerge from different sapient species, including awakened wild creatures. Colonization, migration, and technological decline remain possible histories within that cosmology; their place in the setting is not settled.

### Vertical layers: the overworld, the celestial heights, and the underworld

**Author's lore premise:** the world is not limited to one surface. A celestial (sky) layer of floating islands lies far above the overworld, and an underworld of caverns lies beneath it. Both are compact compared with the overworld.

- **Overworld (Y 0–1024):** the main generated world. Underground rock fills 0–256, the oceans span 256–384, sea level is 384, and mountains rise steeply to about 896. The rest is open sky up to the build limit.
- **Celestial heights (from about Y 2048, roughly 256 blocks tall):** a mini-world of floating islands. Reaching it must not depend on building. Construction above about Y 1024 fails for a lore reason that is still undecided; in game it is a hard build limit. The player must find another way to climb the 1024–2048 gap: a skill, an ability, a creature, a structure, or a phenomenon.
- **Underworld (below Y 0, roughly 256 blocks deep):** a separate subterranean layer. Large caverns alternate with narrow passages. Its floor is molten lava above an unbreakable bottom layer that ends the world.

The layers should feel distinct in terrain, light, materials, and inhabitants, not just be relocated copies of the overworld. The underworld has no sunlight, so light comes mainly from lava and other sources. The celestial heights stay lit by the sun but should not shadow the overworld far below.

**Proposed technical direction:** treat each layer as its own vertical band with separate save data, lighting, and generation. Leave the empty 1024–2048 gap unstored and only pass through it. Unlike the overworld's height-map terrain, islands and caverns need 3D generation. The underworld is enclosed and likely needs no distant-terrain rendering. Islands need their own. Systems built before these layers exist should avoid assuming a single Y range that starts at zero.

### Primordial fox and the origin of life

**Author's lore premise:** an unnamed, large, white, fluffy fox is a primordial being. She disperses herself and spreads her energy through the world, after which life and societies develop. Humans are one expression of this shared origin, alongside elves, dwarves, goblins, slimes, and other creatures. Wild creatures can accumulate enough energy to become sapient and establish civilizations of their own.

Clicking “Create World” could represent this dispersal, followed by procedural development and history. Whether the fox is gone, dormant, still conscious, or able to return is undecided. The creation sequence is a lore concept, not a requirement for an expensive animation or a full physical simulation of creation.

The latest scope excludes individually simulated insects. Their appearance in the earlier brainstorming is not a commitment to add them. Chickens, pigs, rabbits, and similar animals illustrate the lower development bands instead.

**Proposed connection to world generation:** establish the physical world and, if useful, an initial distribution of primordial influence; populate life; allow sapient communities to emerge; then simulate settlement, exchange, conflict, collapse, and notable lives before play. This is a dependency outline rather than a fixed evolutionary order. Not every world needs every species or civilization, and an energy map affecting biomes is still only a suggestion from the attached discussion.

### Level, energy, body, and sapience

**Author's proposed model:** level represents a being's development through the fox's energy, with important thresholds for sapience, bodily reinforcement, longevity, and extraordinary abilities.

**Suggested clarification, not a finalized formula:** distinguish several properties rather than making level a universal combat score:

- **Development level:** a signed progression scale describing how far a being has developed its primordial potential.
- **Body and condition:** species anatomy, size, muscle, age, injury, disease, exhaustion, and any acquired reinforcement.
- **Learned capabilities:** knowledge, practiced skills, equipment use, and developed supernatural abilities.
- **Situational advantages:** preparation, surprise, range, numbers, terrain, and available tools.

A lion below level zero can therefore have much greater raw strength than an ordinary human above zero. Sapience enables different reasoning and social capabilities; it does not automatically grant a stronger body. Likewise, a talented merchant can have advanced development without becoming the best fighter or a legitimate ruler.

Negative levels describe a position on the development scale, not a physically negative quantity of energy. If abilities consume a reserve, that reserve should be distinguished from development level. Whether such a reserve exists, how energy is gained or transferred, and whether development can regress remain open.

### Proposed development bands

The following preserves the author's numerical sketch. Ranges are written without overlapping endpoints for clarity; thresholds, growth rates, and final balance remain provisional.

| Development level | Author's proposed meaning |
| --- | --- |
| -100 to below -50 | Lower development, illustrated by animals such as chickens, pigs, and rabbits. Species-specific physical traits still matter. |
| -50 to below 0 | More capable wild creatures, such as wolves or lions. Dangerous bodies do not require human-like sapience. |
| 0 | Awakening to human-comparable sapience. A humanoid transformation or the ability to assume a humanoid form is under consideration. |
| 0 to below 50 | Ordinary sapient beings, including humans, elves, and dwarves. |
| 50 to below 100 | Exceptional individuals, potentially distinguished through trade, leadership, craft, or other pursuits. Social rank is not an automatic level reward. |
| 100 | A breakthrough with substantial bodily reinforcement. Its relationship to species differences and combat rules needs definition. |
| Above 100 to below 200 | Further physical development, slower aging, and possible auras that help or influence nearby beings. |
| 200 to below 500 | Agelessness becomes possible in this band. The exact unlock point is unresolved; injury and death remain possible. |
| 500 | A transition toward demigod-like existence. |
| Above 500 to below 1000 | One distinctive ability expressing an aspect of the primordial fox, selected in relation to the individual's personality, conduct, and desires. Examples include short-range teleportation or mind reading. |
| 1000 to 2000 | Intentionally undefined. Multiple aspects, influence over natural laws, and world-changing powers were suggestions in the attached chat, not accepted rules. |

The attached response's language of a “structured soul,” stabilization at exactly level 200, disease resistance, and particular aura effects should likewise be treated as optional interpretations. They are not required lore or mechanics yet.

### Vulnerability and meaningful power

The author wants higher development to matter substantially while preserving the possibility of defeat through numbers, preparation, firearms, traps, disease, or exhaustion. Agelessness means freedom from aging, not invulnerability or automatic resurrection. An aspect does not implicitly protect against every unrelated hazard.

There is one unresolved tension: the initial level-100 rule says a lower-level person cannot win an unarmed duel, while the broader goal permits clever lower-level opponents to win. A proposed reconciliation is to make a healthy, reinforced individual overwhelmingly favored in a straightforward unarmed contest while still resolving actual encounters through bodies, skills, defenses, and circumstances. This is a proposed interpretation, not an adopted absolute combat rule.

Hazards should also obey consistent defenses: a trap or illness can kill a powerful being if it overcomes their actual protection or exploits a vulnerability. High level should neither erase danger nor become meaningless because every ordinary hazard ignores physical reinforcement.

### Emergent skills and primordial aspects

**Confirmed direction:** retain skill trees and procedural combinations from the earlier vision. The attached suggestion that all abilities replace trees with purely emergent unlocks is an alternative, not an agreed change.

**Proposed combined model:** ordinary practice and teaching develop skills within understandable trees; significant experiences, repeated behavior, circumstances, personality, and desires can trigger special variants or unlocks. Skills can improve, branch, or combine under defined compatibility rules. Illustrative possibilities include survival instincts after repeated escapes, an aptitude for deals after trading, or regeneration developing into a more adaptive form. These examples are not a fixed content list.

At the demigod threshold, one personal aspect could develop from that history and express a recognizable quality of the fox. Whether the aspect is a transformed existing skill or a separate ability is open. Receiving one aspect does not imply losing all ordinary skills, and a basic skill such as reading emotions need not be identical to an aspect granting supernatural mind reading.

The previous response proposed strict gates of ordinary skills below 100, energy-enhanced skills above 100, and aspects above 500. Only the broad breakthrough/aspect direction is established by the author's sketch; exact restrictions on early magic and special skills are undecided.

The emergence of sapience and powerful individuals should affect history through concrete actions: new communities, teaching, alliances, migration, conquest, or protection. Civilization collapse should still have causal explanations such as lost expertise, conflict, or resource failure. The suggestion that all decline directly reflects energy loss is not established lore.

### Visibility, exploration, and mobility

Controlled characters have a vision radius. Unseen territory is hidden or dimmed, with a distinction between unexplored terrain and previously observed information. Knowledge about distant events should arrive through an appropriate channel.

Travel and relocation require carrying people, possessions, and supplies. Existing settlements and distant actors must retain continuity when their detailed regions unload. Exact map transitions, continuous travel boundaries, and information shared between separated companions remain open.

### Light, night, and great beacons

Light is part of how places are read from afar. Torches and lamps are common: a town at night shows as a cluster of warm points and a soft glow across the land. Lighthouses with roughly kilometre-long reach belong to large cities and important territories, and guide travel across the sea.

Kilometre-scale beacons and light-emitting megastructures are very rare achievements rather than something the player places freely. They are expensive to sustain in lore and to simulate (each one needs its own shadow maps), so their number near any territory stays small. A world rule may enforce that, for example celestial laws that punish a territory for gathering too many great lights.

### People, agency, and ownership

People have needs, personalities, relationships, knowledge, possessions, and progression. They should remain actors with interests even when the player employs, governs, or travels with them.

Items and other assets can belong to individuals or groups. An automatically shared colony inventory is not the universal economic model. Transfer of property needs a social basis such as agreement, trade, authority, or theft. Inheritance, wages, taxation, communal ownership, and enforcement are possible mechanisms to design, not all fixed requirements for the first version.

Player authority must have limits that fit their role. Direct character control, companion orders, management commands, and institutional authority need distinct rules; the exact boundary between them is unresolved.

### Settlement management and political leadership

Two initial paths are under consideration:

- Found a settlement and develop its institutions.
- Enter an existing society and rise through its social or political hierarchy, potentially becoming king, queen, or another leader.

A generalized model should support both player and NPC societies. Titles should correspond to responsibilities and decision-making power rather than only demands for expensive accommodation.

Capable leaders can reduce micromanagement by proposing or authorizing housing, defensive outposts, resource priorities, military expeditions, and other public projects. Such decisions must account for available labor, ownership, resources, and legitimate authority. A leader's personality and competence matter: good leadership is useful, while greed, jealousy, incompetence, or cruelty can cause problems. The player should understand who made a decision and why.

### Causal events and strategic behavior

An example raid chain is: scouts discover an outpost, information reaches a society, a leader evaluates motives and risk, supplies and troops are gathered, and an expedition travels to the target.

Defeat should change future decisions according to what survivors, observers, or other information sources report. A poorly equipped group should not repeatedly sacrifice armies against overwhelming technology without a reason. It might retreat, negotiate, prepare differently, seek allies, or abandon the objective. NPC knowledge should have limits too.

Animal migration can follow food and seasons; caravans can follow demand and route safety. Even unusual arrivals or falling cargo should have a world explanation.

### Knowledge, technology, and refinement

Support progression from simple tools toward advanced technology, potentially including space-related capabilities. Technology is knowledge held by qualified people and preserved in records, rather than a single permanent colony unlock list.

A newcomer may teach what they actually understand; citizenship in an advanced society is not equivalent to expertise. Knowledge can spread through teaching, records, practice, and contact. It can be lost if specialists die and usable records or trained successors do not remain.

Distinguish knowing a design from having the skill, tools, materials, and infrastructure to build it. Further research can refine an existing technology, such as reducing energy use or material requirements. Exact research rules and long-term technological limits are open.

### Mining, materials, and crafting

Vertical deposits should make excavation and material choice meaningful. Ore can support tools, weapons, armor, ornaments, and trade. Production connects geology, access, labor, expertise, and ownership. A proposed quality model combines material properties and maker skill; its exact formula is not decided.

### Trees and great trees

Direction from the author (recorded 2026-11-17; technical plan in `docs/handoff/trees.md`):

- **From shrubs to world trees.** Trees range from thin mountain pines to rare world-scale landmarks, a handful per continent, with trunks up to about 128 m wide and heights up to about 120 m. A tree never grows into a ceiling or past the build limit; it spreads instead, so trees also grow indoors and on mountain tops.
- **Trees are terrain.** Trunks and limbs thick enough are blocks that can be cut, climbed, and built upon; leaves and thin twigs are not. Peoples such as elves can make homes on and in great trees.
- **Cutting and regrowth.** Without physics, cutting a trunk or limb removes what is above the cut, and the tree regrows from the cut. Whether a stump resprouts depends on the species. Once physics exists, a cut tree should fall instead.
- **Death leaves the wood.** When a tree dies, its leaves fall but all its wood stays. Some peoples (for example elves) may live on great trees and prolong their lives; other species that inhabit tree houses do not care. How a settlement reacts when its tree dies is open.
- **Growth accelerators.** Growth is compressed by size (small trees mature in about a game year, massive ones in centuries). Fertilizers and special abilities can speed up growth and possibly extend a tree's life; details are open.

### Temperature, fluids, and phase changes

Temperature should vary locally with weather, depth, insulation, heat sources, and surrounding materials. Active geysers can warm nearby areas; sheltered deep spaces can behave differently from exposed terrain. Cave temperature should follow the chosen environmental model rather than always being colder by definition.

Fluids should support elevation, connected volumes, pressure effects, wells, aquifers, and dangerous flooding. Freezing and thawing should be gradual consequences of local conditions. Thin ice can break and create drowning hazards; a calendar transition should not instantly turn an entire occupied body of water solid.

The numerical model, resolution, update rate, and performance limits are implementation decisions still to make.

### Taverns, visitors, and social exchange

Taverns and other gathering places should attract travelers and guests. Visitors socialize, form relationships, and bring information that connects local life to the wider world. Trade, recruitment, teaching, and rumors are possible interactions. These spaces should feel like part of an inhabited society rather than only a service menu.

### RPG progression, skill trees, and procedural combinations

Progression applies throughout the world, including NPCs and animals. Support personal stats, development through activity, progression trees, and special abilities unlocked under appropriate conditions. The proposed development bands, awakening, and aspects above provide a lore framework; exact stat lists, point allocation, classes, and unlock rules are open.

Skills should be able to combine procedurally. A proposed implementation direction is to compose defined effects and conditions with compatibility rules, costs, and clear descriptions. The player must be able to understand a generated ability; the final combination model is not chosen.

Rare legendary individuals can become historical figures, exceptional allies, or threats that demand preparation. Their importance should arise from capability and actions in the world, with reports or evidence helping the player judge the danger.

### Time and perception abilities

Time slowing, time manipulation, and heightened perception belong in the intended ability space. Their exact distinction is unresolved: an ability might change world time, relative action rates, the player's decision window, or perceived information.

These mechanics need explicit rules for duration, costs, affected actors, projectiles, AI, and environmental simulation. Whether NPCs can acquire equivalent abilities and how the player perceives their use remain open. Single-player is a firm constraint; the mechanic itself should be prototyped early enough to inform the simulation clock.

### World time, reaction speed, and perception

Direction from the author (recorded 2026-11-17; details open):

- **Day length.** One game day lasts 72 real minutes: exactly 5% of a real day. About 30 minutes of a day are meant to be experienced by the player, and the remaining night is mostly skipped through sleep. The clock is continuous (fractional game days) with no base tick unit, unlike Minecraft's ticks; one game second is `DayLengthSeconds / 86400` real seconds (0.05 s at 72 minutes).
- **Configurable later, not now.** Day length may become a player setting (some may find 72 minutes too long or too short). Decide this only once the game is playable with time-based skills and settlement/NPC simulation.
- **Decision timing is separate from the clock.** A game second (0.05 s) is too short for a player to react to anything. Decision windows are expressed in real time instead: an ordinary NPC makes a decision about once per real second.
- **Reaction speed is a stat.** Strong enemies decide faster, down to fractions of a millisecond for very strong ones. The length of the chain *thought about attacking -> attack started -> attack ended* also follows the actor's stats (agility and similar).
- **Time abilities are perception.** A time-based ability makes time appear to flow slower for the player: the player still moves at the pace of world time and gets no instant movement; they gain a longer decision window and see more. This is the basis for answering the open time-ability decision below.

## 6. Technical Direction

Use the existing C++20 foundation, EnTT for appropriate actor/component systems, chunk-based terrain, and worker jobs for suitable independent work. ECS supports organizing data and behavior; autonomous societies still require actual decision, economy, and world-simulation systems. Terrain need not become one entity per voxel.

Separate simulation state from rendering and expose clear ownership and synchronization boundaries. Detailed local simulation, distant world activity, AI, and environmental updates can use different frequencies while preserving meaningful consequences across transitions.

Performance should be demonstrated with representative workloads and profiling. Hardware targets, actor counts, active terrain size, memory limits, and frame/tick budgets still need measurable goals. Broad physical realism should be approximated where necessary to preserve the intended gameplay.

## 7. Proposed Development Priorities

This sequence replaces the earlier outline that postponed RPG progression until after politics. It is a direction for incremental work, not a promise that every stage is already implemented.

| Stage | Playable outcome | Main questions it should resolve |
| --- | --- | --- |
| 1. Character survival slice | One character moves through a small vertical region, sees nearby terrain, gathers a resource, and satisfies a basic need. | Viewpoint, controls, pathfinding, visibility, and a headless-testable simulation loop. |
| 2. Progression and ability slice | The character improves through play, unlocks a skill, and experiments with a constrained skill combination and a time/perception prototype. | Development versus body stats, effect composition, and simulation-time semantics. |
| 3. Social settlement slice | A few autonomous inhabitants own goods, work, exchange resources, and maintain a small settlement. | Player authority, individual agency, founding versus joining, and useful management commands. |
| 4. Travel and continuity slice | The player leaves and returns while a settlement and neighboring group continue acting. | Distant simulation, relocation, information flow, and one explainable event chain; later, continuity for an awakened species. |
| 5. Delegation and knowledge slice | A leader handles one recurring responsibility; an expert teaches or records useful knowledge. | Valuable leadership, permissions, institutional differences, and knowledge loss. |
| 6. Environmental and historical depth | Local heat/fluid interactions and longer histories enrich the working loop. | Sustainable simulation cost, gradual freezing, persistent consequences, and legendary lives. |

Maintain performance and regression testing throughout. Prioritize crucial gameplay rules under `tests/` as described in [tests/AGENTS.md](tests/AGENTS.md). Extend content and simulation depth after the small interacting systems produce a compelling experience.

## 8. Open Decisions

Decide these when their first prototype requires an answer:

- **Player identity:** one persistent protagonist, switchable controlled people, or a protagonist with delegated companions? What happens after death?
- **Control and management:** what can be ordered directly, what needs consent or authority, and what can leaders execute automatically?
- **Settlement entry:** implement founding or joining first, and which shared rules must already support both?
- **Time abilities:** perception versus physical time effects, targeting, and NPC access. Current direction: perception (a longer decision window), with reaction speed as an actor stat (see *World time, reaction speed, and perception*).
- **Day length setting:** whether players may change the 72-minute day, decided once time-based skills and settlement simulation are playable.
- **Progression:** stats and points, class/tree structure, combination constraints, and how exceptional power interacts with social authority.
- **Lore and progression:** the fox's name and fate; sources and transfer of energy; fixed versus conditional thresholds; humanoid transformation versus optional shapeshifting; aspect selection; the level-100 combat promise; and the undefined 1000–2000 band.
- **Society and awakening:** how sapience develops into language, education, institutions, and recognition by other societies. Individual awakening need not instantly produce a functioning civilization.
- **World form:** continuous versus linked local regions, how the fox-origin sequence is presented, technological range, and how far generated history must go initially.
- **Vertical layers:** names for the celestial and underworld layers; the lore reason building fails above Y 1024; how the 1024–2048 gap is crossed and when in progression; how the underworld is entered; whether either layer has its own species, resources, or history; and whether layer connections occur everywhere or only at specific sites.
- **Great lights:** how kilometre-scale beacons are obtained and maintained, and whether a lore rule (such as celestial laws against gathering too many great lights in one territory) limits them.
- **Presentation:** final camera and art style; the original 2D readability goal must be reconciled with the current 3D voxel renderer.
- **Performance targets:** minimum hardware and representative simulation workloads.

## 9. Design Tensions to Evaluate

Personal power should coexist with a meaningful settlement economy and political life. Autonomy should reduce micromanagement while keeping decisions understandable and influenceable. Causal events need enough visible evidence to feel fair without giving the player omniscience. Procedural abilities need expressive combinations without unbounded complexity. Detailed simulation must fit the low-end performance goal.

Evaluate new features by how they strengthen these experiences and interact with existing systems. This vision establishes the game's identity; it does not require building every proposed subsystem before the first enjoyable version.
