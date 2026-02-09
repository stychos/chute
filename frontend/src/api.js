const AUTH_KEY = 'chute_auth'

export function getAuth() {
  return sessionStorage.getItem(AUTH_KEY) || ''
}

export function setAuth(password) {
  if (password) {
    sessionStorage.setItem(AUTH_KEY, btoa(':' + password))
  } else {
    sessionStorage.removeItem(AUTH_KEY)
  }
}

export function clearAuth() {
  sessionStorage.removeItem(AUTH_KEY)
}

function headers() {
  const h = { 'Accept': 'application/json' }
  const auth = getAuth()
  if (auth) h['Authorization'] = 'Basic ' + auth
  return h
}

export async function api(url, opts = {}) {
  const res = await fetch(url, {
    ...opts,
    headers: { ...headers(), ...opts.headers }
  })
  if (res.status === 401) throw new Error('auth')
  return res
}

export async function apiGet(url) {
  const res = await api(url)
  return res.json()
}

export async function apiPost(url, body) {
  return api(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  })
}
