/**
 * PebbleKit JS — fetches current conditions + today's hi/lo from
 * Open-Meteo (free, no API key) and sends them to the watch.
 *
 * Requires package.json:
 *   "capabilities": ["location"],
 *   "messageKeys": ["TEMPERATURE", "TEMP_HIGH", "TEMP_LOW", "CONDITIONS", "REQUEST_WEATHER"],
 *   "enableMultiJS": true
 */

var xhrRequest = function (url, type, callback) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    callback(this.responseText);
  };
  xhr.open(type, url);
  xhr.send();
};

/**
 * Convert WMO weather codes to short human-readable strings.
 * See: https://open-meteo.com/en/docs
 */
function weatherCodeToCondition(code) {
  if (code === 0) return 'Clear';
  if (code <= 3) return 'Cloudy';
  if (code <= 48) return 'Fog';
  if (code <= 55) return 'Drizzle';
  if (code <= 57) return 'Fz. Drizzle';
  if (code <= 65) return 'Rain';
  if (code <= 67) return 'Fz. Rain';
  if (code <= 75) return 'Snow';
  if (code <= 77) return 'Snow Grains';
  if (code <= 82) return 'Showers';
  if (code <= 86) return 'Snow Shwrs';
  if (code <= 99) return 'T-Storm';
  return 'Unknown';
}

function locationSuccess(pos) {
  var url = 'https://api.open-meteo.com/v1/forecast?' +
      'latitude=' + pos.coords.latitude +
      '&longitude=' + pos.coords.longitude +
      '&current=temperature_2m,weather_code' +
      '&daily=temperature_2m_max,temperature_2m_min' +
      '&temperature_unit=fahrenheit' +
      '&timezone=auto';

  xhrRequest(url, 'GET', function (responseText) {
    var json = JSON.parse(responseText);
    var temperature = Math.round(json.current.temperature_2m);
    var conditions = weatherCodeToCondition(json.current.weather_code);
    var tempHigh = Math.round(json.daily.temperature_2m_max[0]);
    var tempLow = Math.round(json.daily.temperature_2m_min[0]);

    var dictionary = {
      'TEMPERATURE': temperature,
      'TEMP_HIGH': tempHigh,
      'TEMP_LOW': tempLow,
      'CONDITIONS': conditions
    };

    Pebble.sendAppMessage(dictionary,
      function () { console.log('Weather sent to Pebble successfully!'); },
      function () { console.log('Error sending weather to Pebble!'); }
    );
  });
}

function locationError(err) {
  console.log('Error requesting location: ' + err);
}

function getWeather() {
  navigator.geolocation.getCurrentPosition(
    locationSuccess,
    locationError,
    { timeout: 15000, maximumAge: 60000 }
  );
}

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready!');
  getWeather();
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload['REQUEST_WEATHER']) {
    getWeather();
  }
});
