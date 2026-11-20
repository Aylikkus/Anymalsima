# Caverns and underground water

Future overworld cavern generation should connect to the water systems in
[steps 4 and 5 of the water handoff](water.md#4-drain-and-refill-standing-bodies-with-lazy-reconciliation).
Caverns can receive water from the surface and have standing water and flowing
channels of their own.

## Drainage from the surface

If a player opens a downhill passage from a lake or sea into a cavern, water
should flow through the opening while the source surface is high enough to feed
it. Each transfer debits the source and credits the cavern by the same amount.
The cavern can fill, equalize with the source, or spill into a lower passage.
The source can also recede below the opening and cut off the flow.

Confirm connections through the actual three-dimensional passages. A chamber
beneath a lake can stay dry while solid rock separates them. If an opening leads
into an unverified part of a cavern, defer flow beyond the known space until
there is enough evidence. Confirmed connections can exchange water while the
intervening chunks are unloaded.

## Water generated inside caverns

Generate underground lakes and pools as standing bodies with storage and spill
heights. Springs can feed underground streams that drain into these pools,
continue through lower passages, or emerge at the surface. Use the same water
accounting and flow rules for these bodies and channels. A player who opens or
seals a passage can change which chambers fill and where the water goes.

## Underworld

The separate underworld described in [IDEA.md](../../IDEA.md#vertical-layers-the-overworld-the-celestial-heights-and-the-underworld)
can use the same fluid rules for its caverns and molten floor. Whether water can
cross between layers, and what happens if it reaches lava, can be decided when
the underworld is designed.
