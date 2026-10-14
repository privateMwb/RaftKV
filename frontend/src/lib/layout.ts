export interface Point {
  /** Percent of the container's width. */
  x: number;
  /** Percent of the container's height. */
  y: number;
}

/**
 * Node positions on a ring, by index, so a card never moves when roles change
 * (a leader election should recolour cards, not shuffle them). The first node
 * sits at the top; the rest follow clockwise. Coordinates are percentages of the
 * container, centred on the card.
 */
export function ringPositions(count: number): Point[] {
  if (count <= 0) return [];
  if (count === 1) return [{ x: 50, y: 50 }];
  if (count === 2)
    return [
      { x: 25, y: 50 },
      { x: 75, y: 50 },
    ];

  const radiusX = 33;
  const radiusY = 30;
  const points: Point[] = [];

  for (let i = 0; i < count; i += 1) {
    const angle = -Math.PI / 2 + (i * 2 * Math.PI) / count;
    points.push({
      x: round(50 + radiusX * Math.cos(angle)),
      y: round(50 + radiusY * Math.sin(angle)),
    });
  }

  return points;
}

function round(value: number): number {
  return Math.round(value * 100) / 100;
}
