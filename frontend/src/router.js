import { createRouter, createWebHashHistory } from 'vue-router'
import PlayerView from './views/PlayerView.vue'
import ConfigPanel from './views/ConfigPanel.vue'
import StatusTab from './views/config/StatusTab.vue'
import WiFiTab from './views/config/WiFiTab.vue'
import AudioTab from './views/config/AudioTab.vue'
import CameraTab from './views/config/CameraTab.vue'
import FlashTab from './views/config/FlashTab.vue'
import PasswordTab from './views/config/PasswordTab.vue'
import FirmwareTab from './views/config/FirmwareTab.vue'

const routes = [
  {
    path: '/',
    component: PlayerView,
    children: [
      {
        path: 'config',
        component: ConfigPanel,
        redirect: '/config/status',
        children: [
          { path: 'status', component: StatusTab },
          { path: 'wifi', component: WiFiTab },
          { path: 'audio', component: AudioTab },
          { path: 'camera', component: CameraTab },
          { path: 'flash', component: FlashTab },
          { path: 'password', component: PasswordTab },
          { path: 'firmware', component: FirmwareTab },
        ]
      }
    ]
  }
]

export default createRouter({
  history: createWebHashHistory(),
  routes
})
