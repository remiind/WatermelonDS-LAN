package me.magnum.melonds.domain.model.layout

import me.magnum.melonds.domain.model.Rect

data class PositionedLayoutComponent(
    val rect: Rect,
    val component: LayoutComponent,
    val alpha: Float = 1f,
    val onTop: Boolean = false,
    val buttonMode: VirtualButtonMode = VirtualButtonMode.NORMAL,
) {

    fun isScreen(): Boolean {
        return component.isScreen()
    }
}
